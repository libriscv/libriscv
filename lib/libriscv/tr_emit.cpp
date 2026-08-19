#include "machine.hpp"
#include "decoder_cache.hpp"
#include "instruction_list.hpp"
#include <array>
#include <inttypes.h>
#include <optional>
#include "rv32i_instr.hpp"
#include "rvfd.hpp"
#include "tr_types.hpp"
#ifdef RISCV_EXT_C
#include "rvc.hpp"
#endif
#ifdef RISCV_EXT_VECTOR
#include "rvv.hpp"
#endif

#define PCRELA(x) ((address_t) (this->pc() + (x)))
#define PCRELS(x) hex_address(PCRELA(x)) + "LL"
#define STRADDR(x) (hex_address(x) + "LL")
// Reveal PC on unknown instructions
// libtcc always runs on the current machine, so we can use the handler index directly
#define UNKNOWN_INSTRUCTION() { \
  this->invalidate_all_bounds_checks(); \
  if (tinfo.is_libtcc) { \
	if (!instr.is_illegal()) { \
		this->store_loaded_registers(); \
		const uintptr_t handler = (uintptr_t)CPU<W>::decode(instr).handler; \
		code += "if (api.execute_handler(cpu, " + std::to_string(instr.whole) + ", " + std::to_string(handler) + "))\n" \
			"  RETURN_VALUES(0, 0);\n"; \
		this->reload_all_registers(); \
	} else if (m_zero_insn_counter <= 1) { \
		code += "api.exception(cpu, " + STRADDR(this->pc()) + ", ILLEGAL_OPCODE);\n"; \
		code += "RETURN_VALUES(0, 0);\n"; \
	} \
  } else { \
	if (!instr.is_illegal()) { \
		this->store_loaded_registers(); \
		code += "#ifdef __wasm__\n"; \
		code += "api.execute(cpu, " + std::to_string(instr.whole) + ");\n"; \
		code += "#else\n"; \
		code += "{ static int handler_idx = 0;\n"; \
		code += "if (handler_idx) api.handlers[handler_idx](cpu, " + std::to_string(instr.whole) + ");\n"; \
		code += "else handler_idx = api.execute(cpu, " + std::to_string(instr.whole) + "); }\n"; \
		code += "#endif\n"; \
		this->reload_all_registers(); \
	} else if (m_zero_insn_counter <= 1) \
		code += "api.exception(cpu, " + hex_address(this->pc()) + ", ILLEGAL_OPCODE);\n"; \
  } \
}
// NOTE: The handler is arbitrary emulator code that can write any register,
// so every bounds-check window dies here.
#define WELL_KNOWN_INSTRUCTION() { \
  this->invalidate_all_bounds_checks(); \
  if (tinfo.is_libtcc) { \
	const uintptr_t handler = (uintptr_t)CPU<W>::decode(instr).handler; \
	code += "if (api.execute_handler(cpu, " + std::to_string(instr.whole) + ", " + std::to_string(handler) + "))\n" \
		"  RETURN_VALUES(0, 0);\n"; \
  } else { \
	code += "#ifdef __wasm__\n"; \
	code += "api.execute(cpu, " + std::to_string(instr.whole) + ");\n"; \
	code += "#else\n"; \
    code += "{ static int handler_idx = 0;\n"; \
    code += "if (handler_idx) api.handlers[handler_idx](cpu, " + std::to_string(instr.whole) + ");\n"; \
    code += "else handler_idx = api.execute(cpu, " + std::to_string(instr.whole) + "); }\n"; \
	code += "#endif\n"; \
  } \
}

namespace riscv {
static const std::string LOOP_EXPRESSION = "LIKELY(ic < max_ic)";
static const std::string SIGNEXTW = "(int32_t)";
static constexpr int ALIGN_MASK = (compressed_enabled) ? 0x1 : 0x3;

static std::string hex_address(uint64_t addr) {
	char buf[64];
	if (const int len = snprintf(buf, sizeof(buf), "0x%" PRIx64, uint64_t(addr)); len > 0)
		return std::string(buf, len);
	throw MachineException(INVALID_PROGRAM, "Failed to format address");
}

template <int W>
static std::string funclabel(const std::string& func, uint64_t addr) {
	char buf[64];
	if (const int len = snprintf(buf, sizeof(buf), "%s_%" PRIx64, func.c_str(), addr); len > 0)
		return std::string(buf, len);
	throw MachineException(INVALID_PROGRAM, "Failed to format function label");
}
#define FUNCLABEL(addr) funclabel<W>(func, addr)

struct BranchInfo {
	bool sign;
	bool ignore_instruction_limit;
	uint64_t jump_pc;
	uint64_t call_pc;
};

template <int W>
struct Emitter
{
	static constexpr bool OPTIMIZE_SYSCALL_REGISTERS = true;
	static constexpr unsigned XLEN = W * 8u;
	static constexpr int CACHED_REGISTERS = 18; // Number of registers to cache
	using address_t = address_type<W>;
	using saddr_t = signed_address_type<W>;

	bool uses_register_caching() const noexcept { return tinfo.use_register_caching; }

	Emitter(const TransInfo<W>& ptinfo)
		: m_pc(ptinfo.basepc), tinfo(ptinfo)
	{
		this->func = funclabel<W>("f", this->pc());
		this->m_arena_hex_address = hex_address(tinfo.arena_ptr) + "L";

		if (ptinfo.use_automatic_nbit_address_space) {
			// Calculate the encompassing arena bits, which is the highest bit set in the arena size
			int encompassing_arena_bits = 0;
			for (uint64_t i = 1; i < ptinfo.arena_size; i <<= 1)
				encompassing_arena_bits++;
			this->m_encompassing_arena_mask = (1ULL << encompassing_arena_bits) - 1;
		}
	}

	template <typename ... Args>
	void add_code(Args&& ... addendum) {
		([&] {
			this->code += std::string(addendum) + "\n";
		}(), ...);
	}
	const std::string& get_code() const noexcept { return this->code; }

	std::string loaded_regname(int reg) {
		return "reg" + std::to_string(reg);
	}
	void load_register(int reg) {
		if (uses_register_caching()) {
			if (LIKELY(reg != 0 && reg < CACHED_REGISTERS))
				gpr_exists[reg] = true;
		}
	}
	void potentially_reload_register(int reg) {
		// The value came back from memory and may differ from what we checked,
		// regardless of whether register caching made this emit anything.
		this->invalidate_bounds_checks(reg);
		if (uses_register_caching()) {
			if (reg != 0 && reg < CACHED_REGISTERS) {
				add_code(loaded_regname(reg) + " = cpu->r[" + std::to_string(reg) + "];");
			}
		}
	}
	void potentially_realize_register(int reg) {
		if (uses_register_caching()) {
			if (reg != 0 && reg < CACHED_REGISTERS) {
				add_code("cpu->r[" + std::to_string(reg) + "] = " + loaded_regname(reg) + ";");
			}
		}
	}
	void potentially_realize_registers(int x0, int x1) {
		if (uses_register_caching()) {
			for (int reg = x0; reg < x1; reg++) {
				if (reg != 0 && reg < CACHED_REGISTERS) {
					add_code("cpu->r[" + std::to_string(reg) + "] = " + loaded_regname(reg) + ";");
				}
			}
		}
	}

	void reload_all_registers() {
		this->invalidate_all_bounds_checks();
		// Use the LOAD_REGS macro to restore the registers
		if (uses_register_caching())
			add_code("LOAD_REGS_" + this->func + "();");
	}
	void store_loaded_registers() {
		// Use the STORE_REGS macro to store the registers
		if (uses_register_caching())
			add_code("STORE_REGS_" + this->func + "();");
	}
	void reload_syscall_registers() {
		// A non-clobbering system call only writes back A0/A1
		this->invalidate_bounds_checks(10);
		this->invalidate_bounds_checks(11);
		// Use the LOAD_SYS_REGS macro to restore registers modified by a syscall
		if (uses_register_caching())
			add_code("LOAD_SYS_REGS_" + this->func + "();");
	}
	void store_syscall_registers() {
		// Use the STORE_SYS_REGS macro to store registers used by a syscall
		if (uses_register_caching()) {
			add_code("STORE_SYS_REGS_" + this->func + "();");
			this->m_used_store_syscalls = true;
		}
	}

	void exit_function(const std::string& new_pc, bool add_bracket = false)
	{
		this->store_loaded_registers();
		const char* return_code = (tinfo.ignore_instruction_limit) ? "RETURN_VALUES(0, max_ic);" : "RETURN_VALUES(ic, max_ic);";
		add_code(
			(new_pc != "cpu->pc") ? "cpu->pc = " + new_pc + ";" : "",
			return_code, (add_bracket) ? " }" : "");
	}

	std::string from_reg(int reg) {
		if (reg == 3 && tinfo.gp != 0)
			return hex_address(tinfo.gp) + "LL";
		else if (reg != 0) {
			if (auto tracked_value = get_tracked_register(reg); tinfo.is_libtcc && tracked_value) {
				return "(" + hex_address(*tracked_value) + "LL)";
			}
			else if (uses_register_caching() && reg < CACHED_REGISTERS) {
				load_register(reg);
				return loaded_regname(reg);
			} else {
				return "cpu->r[" + std::to_string(reg) + "]";
			}
		}
		return "(addr_t)0";
	}
	std::string from_untracked_reg(int reg) {
		if (reg == 3 && tinfo.gp != 0)
			return hex_address(tinfo.gp) + "L";
		else if (reg != 0) {
			if (uses_register_caching() && reg < CACHED_REGISTERS) {
				load_register(reg);
				return loaded_regname(reg);
			} else {
				return "cpu->r[" + std::to_string(reg) + "]";
			}
		}
		return "(addr_t)0";
	}
	// NOTE: to_reg() is only ever used to name a *destination*, so it is the one
	// choke point every GPR write passes through. Invalidating here means a new
	// instruction cannot silently keep a stale bounds-check window alive.
	std::string to_reg(int reg) {
		if (reg != 0) {
			this->invalidate_bounds_checks(reg);
			if (uses_register_caching() && reg < CACHED_REGISTERS) {
				load_register(reg);
				return loaded_regname(reg);
			} else {
				return "cpu->r[" + std::to_string(reg) + "]";
			}
		}
		return "(addr_t)0";
	}
	std::string from_fpreg(int reg) {
		return "cpu->fr[" + std::to_string(reg) + "]";
	}
#ifdef RISCV_EXT_VECTOR
	std::string from_rvvreg(int reg) {
		return "cpu->rvv.lane[" + std::to_string(reg) + "]";
	}
	// Elements in one full register at this SEW, ie. VLMAX at LMUL=1.
	static constexpr unsigned rvv_full_vl(unsigned vsew) noexcept {
		return VectorLane::size() >> vsew;
	}
	// Condition for addressing exactly one full register: the given SEW,
	// m1, vl == VLMAX and a valid vtype. Matches the whole-group fast path
	// in unit_stride_load/store (rvv_instr.cpp), and is the only case where
	// every element is active and there is no tail. A vsetvli earlier in
	// this block may already have proven SEW/LMUL/vill, leaving only vl.
	std::string rvv_full_lane_condition(unsigned vsew) const {
		// An inlined vsetvli leaves vl in a C local, sparing us a reload.
		std::string cond = (m_vl_local.empty() ? "cpu->rvv.vl" : m_vl_local)
			+ " == " + std::to_string(rvv_full_vl(vsew));
		if (m_vtype.known)
			return cond;
		return cond + " && cpu->rvv.vsew == " + std::to_string(vsew)
			+ " && cpu->rvv.lmul == 0 && !cpu->rvv.vill";
	}
	// The condition as a live C local, computed once per block and shared by
	// every vector instruction in it. Always emitted at statement level, so
	// that the bodies opened below can all see it.
	const std::string& rvv_guard(unsigned vsew)
	{
		auto& guard = m_rvv_guard.at(vsew);
		if (guard.empty()) {
			// With vl known too the guard folds to a constant, and the C
			// compiler drops either the body or the fallback entirely.
			if (m_vtype.known && m_vl_known)
				guard = (m_vl == rvv_full_vl(vsew)) ? "1" : "0";
			else {
				guard = "vok" + PCRELS(0) + "_" + std::to_string(vsew);
				// Declared and assigned separately: an embedded translation
				// is compiled as C++, where a goto may not jump over an
				// initialization, and labels land between vector blocks.
				add_code("int " + guard + "; " + guard + " = "
					+ rvv_full_lane_condition(vsew) + ";");
			}
		}
		return guard;
	}
	// vl and vtype are about to change, or may have changed behind our back.
	void reset_vector_config() {
		this->m_vtype = {};
		this->m_vl_known = false;
		this->m_vl_local.clear();
		for (auto& guard : this->m_rvv_guard) guard.clear();
		for (auto& guard : this->m_rvv_int_guard) guard.clear();
	}
	// Call the interpreter's instruction handler (non-libtcc slow path)
	void emit_vector_handler_call(const rv32i_instruction& instr) {
		code += "#ifdef __wasm__\n";
		code += "api.execute(cpu, " + std::to_string(instr.whole) + ");\n";
		code += "#else\n";
		code += "{ static int handler_idx = 0;\n";
		code += "if (handler_idx) api.handlers[handler_idx](cpu, " + std::to_string(instr.whole) + ");\n";
		code += "else handler_idx = api.execute(cpu, " + std::to_string(instr.whole) + "); }\n";
		code += "#endif\n";
	}

	// The integer registers potentially used by a vector instruction handler
	struct VectorScalarUse {
		int reads[2] { -1, -1 };
		int writes { -1 };
	};
	static VectorScalarUse vector_scalar_use(const rv32i_instruction& vinstr)
	{
		const rv32v_instruction vi { vinstr };
		VectorScalarUse use;
		switch (vinstr.opcode()) {
		case RV32V_OP:
			switch (vinstr.vwidth()) {
			case 0x4: // OPI.VX: scalar operand from x[rs1]
				use.reads[0] = vi.OPVI.imm;
				break;
			case 0x6: // OPM.VX: scalar operand from x[rs1] (VMV.S.X, VSLIDE1*.VX, ...)
				use.reads[0] = vi.OPVV.vs1;
				break;
			case 0x2: // OPM.VV: VWXUNARY0 (VMV.X.S, VCPOP.M, VFIRST.M) writes x[rd]
				if (vi.OPVV.funct6 == 0b010000)
					use.writes = vi.OPVV.vd;
				break;
			case 0x7: // Vector configuration: AVL from x[rs1], resulting vl into x[rd]
				switch (vinstr.vsetfunc()) {
				case 0x0:
				case 0x1: // VSETVLI
					use.reads[0] = vi.VLI.rs1;
					break;
				case 0x2: // VSETVL: new vtype comes from x[rs2] as well
					use.reads[0] = vi.VSETVL.rs1;
					use.reads[1] = vi.VSETVL.rs2;
					break;
				default: // VSETIVLI: the AVL is a 5-bit immediate
					break;
				}
				use.writes = vi.VLI.rd;
				break;
			default: // OPI.VV, OPI.VI, OPF.VV, OPF.VF: vector and fp registers only
				break;
			}
			break;
		case RV32F_LOAD:  // Vector loads address memory through x[rs1]
			use.reads[0] = vi.VL.rs1;
			// A strided access takes its byte stride from x[rs2] too.
			if (vi.VL.mop == 0b10)
				use.reads[1] = vi.VLS.rs2;
			break;
		case RV32F_STORE: // x[rs1] likewise
			use.reads[0] = vi.VS.rs1;
			if (vi.VS.mop == 0b10)
				use.reads[1] = vi.VSS.rs2;
			break;
		default:
			break;
		}
		return use;
	}
	// Realize the integer registers the handler reads.
	void realize_vector_scalar_reads(const VectorScalarUse& use) {
		for (const int reg : use.reads) {
			if (reg > 0) {
				this->load_register(reg);
				this->potentially_realize_register(reg);
			}
		}
	}
	// Reload the one integer register the handler may have written.
	void reload_vector_scalar_writes(const VectorScalarUse& use) {
		if (use.writes > 0) {
			this->reset_tracked_register(use.writes);
			this->potentially_reload_register(use.writes);
		}
	}
	// Hand one vector instruction to the interpreter handler. Callers
	// realize/reload the integer registers around it. The libtcc exception
	// path returns from the block, so cached registers are stored first.
	void emit_vector_handler_invoke()
	{
		if (tinfo.is_libtcc) {
			const uintptr_t handler = (uintptr_t)CPU<W>::decode(instr).handler;
			code += "if (UNLIKELY(api.execute_handler(cpu, " + std::to_string(instr.whole)
				+ ", " + std::to_string(handler) + "))) {\n";
			this->store_loaded_registers();
			code += "RETURN_VALUES(0, 0);\n}\n";
		} else {
			this->emit_vector_handler_call(instr);
		}
	}
	// Run one vector instruction in the interpreter, realizing and
	// reloading only the integer registers the handler names.
	void emit_vector_slowpath()
	{
		if (!this->vector_is_guarded_inlinable(instr))
			this->reset_vector_config();
		const auto use = vector_scalar_use(this->instr);
		this->realize_vector_scalar_reads(use);
		this->emit_vector_handler_invoke();
		this->reload_vector_scalar_writes(use);
	}
	// Whether an arena bounds check is needed at runtime.
	bool vector_memory_needs_bounds_check() noexcept {
		return !tinfo.unsafe_remove_checks && !uses_Nbit_encompassing_arena();
	}
	// The element-wise float operator this encoding computes, or nullptr
	// when it is not translated directly.
	static const char* vector_float_operator(const rv32i_instruction& vinstr)
	{
		if (vinstr.vwidth() != 0x1 && vinstr.vwidth() != 0x5) // OPF.VV / OPF.VF
			return nullptr;
		if (!rv32v_instruction{vinstr}.OPVV.vm)
			return nullptr; // Masked: element activity depends on v0 at runtime
		switch (rv32v_instruction{vinstr}.OPVV.funct6) {
		case 0b000000: return " + "; // VFADD
		case 0b000010: return " - "; // VFSUB
		case 0b100000: return " / "; // VFDIV
		case 0b100100: return " * "; // VFMUL
		default:       return nullptr;
		}
	}
	// True for the eight fused multiply-add encodings shared by OPFVV and
	// OPFVF. They read the destination as their third operand.
	static bool vector_is_float_fma(const rv32i_instruction& vinstr)
	{
		if (vinstr.vwidth() != 0x1 && vinstr.vwidth() != 0x5) // OPF.VV / OPF.VF
			return false;
		if (!rv32v_instruction{vinstr}.OPVV.vm)
			return false; // Masked: element activity depends on v0 at runtime
		return (rv32v_instruction{vinstr}.OPVV.funct6 & 0b111000) == 0b101000;
	}
	// log2(EEW/8) for a unit-stride access width, or -1 for the widths that
	// are not one of the plain 8/16/32/64-bit element forms.
	static int vector_unit_stride_sew(uint32_t width) noexcept
	{
		switch (width) {
		case 0b000: return 0; // VLE8  / VSE8
		case 0b101: return 1; // VLE16 / VSE16
		case 0b110: return 2; // VLE32 / VSE32
		case 0b111: return 3; // VLE64 / VSE64
		default:    return -1;
		}
	}
	// Helpers to inline vector memory forms that are simple enough to be expressed in C
	enum class VectorMemForm {
		None,             // left to the interpreter handler
		UnitStride,       // vle<eew>.v / vse<eew>.v: vl*EEW bytes
		MaskedUnitStride, // ... the same, with v0 choosing the elements
		Strided,          // vlse<eew>.v / vsse<eew>.v: vl elements, x[rs2] apart
		Indexed,          // vlxei<eew>.v / vsxei<eew>.v: vl elements at vs2[i]
		Segment,          // vlseg<nf>e<eew>.v / vsseg: nf fields, interleaved
		WholeRegister,    // vl<n>re<eew>.v / vs<n>r.v: n registers, no vtype
		Mask,             // vlm.v / vsm.v: ceil(vl/8) bytes
	};
	struct VectorMemInfo {
		VectorMemForm form = VectorMemForm::None;
		bool     is_store = false;
		unsigned eew_log2 = 0; // log2(EEW / 8), the unit-stride forms
		unsigned nregs    = 1; // registers moved by a whole-register transfer
		unsigned vreg     = 0; // vd, or vs3 in a store
		unsigned rs1      = 0; // the base address
		unsigned rs2      = 0; // the byte stride, in a strided access
		unsigned vs2      = 0; // the index vector, in an indexed access
	};
	// The longest run any inlined form moves: eight registers, which is both
	// LMUL=8 and the widest whole-register transfer.
	static constexpr uint64_t vector_max_transfer() noexcept {
		return 8ull * VectorLane::size();
	}
	// Classify a vector load or store. Everything decided here is static: nf,
	// mew, mop, lumop, vm and the width field are all fixed by the encoding,
	// so what is left for runtime is only what vtype decides.
	VectorMemInfo vector_memory_form(const rv32i_instruction& vinstr)
	{
		const rv32v_instruction vi { vinstr };
		VectorMemInfo info;
		if (vinstr.opcode() == RV32F_STORE)
			info.is_store = true;
		else if (vinstr.opcode() != RV32F_LOAD)
			return {};
		// Every body below reaches guest memory as a host pointer.
		if (!uses_flat_memory_arena())
			return {};
		// MEW asks for an element wider than 64 bits: that belongs to the
		// handler.
		if (vi.VL.mew)
			return {};
		info.vreg = info.is_store ? vi.VS.vs3 : vi.VL.vd;
		info.rs1  = info.is_store ? vi.VS.rs1 : vi.VL.rs1;
		const unsigned nf = vi.VL.nf + 1; // fields per segment
		if (vi.VL.mop == 0b01 || vi.VL.mop == 0b11) { // Indexed, un/ordered
			if (!vi.VL.vm || nf != 1)
				return {};
			const int ieew = vector_unit_stride_sew(vi.VL.width);
			if (ieew < 0)
				return {};
			info.eew_log2 = unsigned(ieew); // ... the index width, here
			info.vs2 = info.is_store ? vi.VSX.vs2 : vi.VLX.vs2;
			info.form = VectorMemForm::Indexed;
			return info;
		}
		if (vi.VL.mop == 0b10) { // The strided access
			if (!vi.VL.vm || nf != 1)
				return {}; // Masked, or several fields sharing an address
			const int eew = vector_unit_stride_sew(vi.VL.width);
			if (eew < 0)
				return {};
			info.eew_log2 = unsigned(eew);
			info.rs2 = info.is_store ? vi.VSS.rs2 : vi.VLS.rs2;
			info.form = VectorMemForm::Strided;
			return info;
		}
		switch (info.is_store ? vi.VS.sumop : vi.VL.lumop) {
		case 0b00000: { // The plain unit-stride access
			const int eew = vector_unit_stride_sew(vi.VL.width);
			if (eew < 0)
				return {};
			info.eew_log2 = unsigned(eew);
			if (nf != 1) { // Segmented: nf fields share one element index
				if (!vi.VL.vm || nf > 8 || info.vreg + nf > 32)
					return {};
				info.nregs = nf;
				info.form = VectorMemForm::Segment;
				return info;
			}
			info.form = vi.VL.vm ? VectorMemForm::UnitStride
			                     : VectorMemForm::MaskedUnitStride;
			return info; }
		case 0b01000: // vl<n>re<eew>.v / vs<n>r.v
			// An illegal register count, or a group that is not aligned to
			// its own size, has to trap: only the handler knows how.
			if (!vi.VL.vm || (nf != 1 && nf != 2 && nf != 4 && nf != 8))
				return {};
			if ((info.vreg % nf) != 0 || info.vreg + nf > 32)
				return {};
			info.nregs = nf;
			info.form = VectorMemForm::WholeRegister;
			return info;
		case 0b01011: // vlm.v / vsm.v
			if (!vi.VL.vm || nf != 1 || vi.VL.width != 0)
				return {};
			info.form = VectorMemForm::Mask;
			return info;
		default: // Fault-only-first, and the encodings that trap
			return {};
		}
	}
	// True when this instruction is inlined rather than handed over, and so
	// leaves vl and vtype exactly as it found them.
	bool vector_is_guarded_inlinable(const rv32i_instruction& vinstr)
	{
		if (vinstr.opcode() == RV32F_LOAD || vinstr.opcode() == RV32F_STORE)
			return vector_memory_form(vinstr).form != VectorMemForm::None;
		if (vector_is_move_xs(vinstr) && !this->vector_int_sews().empty())
			return true;
		if (vector_int_form(vinstr).op != VectorIntOp::None
			&& !this->vector_int_sews().empty())
			return true;
		return !this->vector_inlinable_sews(vinstr).empty();
	}
	// The SEWs this float arithmetic can be inlined at. It follows vtype's
	// SEW, which is known statically only after an inlined vsetvli; without
	// one, both widths are emitted behind their own guard. Empty means "hand
	// it to the interpreter".
	std::vector<unsigned> vector_inlinable_sews(const rv32i_instruction& vinstr)
	{
		if (vinstr.opcode() != RV32V_OP)
			return {};
		if (vector_float_operator(vinstr) == nullptr && !vector_is_float_fma(vinstr))
			return {};
		// Both float widths are inlinable: a known vtype selects one,
		// otherwise both bodies are emitted behind their own guard.
		if (!m_vtype.known)
			return { 2, 3 };
		// An inline body is only reachable at the SEW and LMUL it was written
		// for, so a known vtype decides it here, not at runtime.
		if (m_vtype.vill || m_vtype.lmul != 0 || m_vtype.vsew < 2)
			return {}; // Fractional/multi-register groups, and no 8/16-bit floats
		return { m_vtype.vsew };
	}
	// The live vl, as a C expression: the local an inlined vsetvli left
	// behind, or the field itself.
	std::string vector_vl_expr() const {
		return m_vl_local.empty() ? "cpu->rvv.vl" : m_vl_local;
	}
	// What a unit-stride transfer still has to establish at runtime, which is
	// what unit_stride_transfer() (rvv_instr.cpp) tests minus whatever a
	// vsetvli earlier in this block has already settled.
	struct VectorStridePlan {
		std::vector<std::string> locals; // declared before the guard is tested
		std::string guard;       // empty when nothing is left to test
		std::string bytes;       // C expression for the length of the run
		bool     bytes_known = false;
		uint64_t known_bytes = 0;
		unsigned dregs = 0;      // registers in the group, 0 when runtime-only
	};
	// Returns false when this encoding cannot be inlined here at all, in
	// which case the caller hands the instruction over whole.
	bool vector_plan_unit_stride(const VectorMemInfo& info, VectorStridePlan& plan)
	{
		constexpr uint64_t lane_size = VectorLane::size();
		const unsigned eew = info.eew_log2;
		if (m_vtype.known) {
			// EMUL = LMUL * EEW / SEW, as a count of registers.
			const int emul = m_vtype.lmul + int(eew) - int(m_vtype.vsew);
			if (m_vtype.vill || emul > 3 || emul < -3)
				return false;
			// A group of one needs no alignment, which covers every EEW at
			// LMUL <= 1; anything wider has to be aligned to its own size.
			plan.dregs = (emul > 0) ? (1u << emul) : 1u;
			if (plan.dregs > 1 && ((info.vreg % plan.dregs) != 0
				|| info.vreg + plan.dregs > 32))
				return false;
			const uint64_t max_bytes = uint64_t(plan.dregs) * lane_size;
			if (m_vl_known) {
				plan.known_bytes = m_vl << eew;
				plan.bytes_known = true;
				// vl outliving the vtype that sized it is rare enough to
				// leave to the handler.
				if (plan.known_bytes > max_bytes)
					return false;
				plan.bytes = std::to_string(plan.known_bytes);
			} else {
				plan.bytes = "vnb" + PCRELS(0);
				plan.locals.push_back("const addr_t " + plan.bytes + " = (addr_t)"
					+ vector_vl_expr() + " << " + std::to_string(eew) + ";");
				plan.guard = plan.bytes + " <= " + std::to_string(max_bytes);
			}
			return true;
		}
		// vtype is live, so the same arithmetic goes into the guard. The
		// destination register is a constant, so the alignment test folds
		// down to a compare once the C compiler knows EMUL.
		const std::string emul = "vem" + PCRELS(0);
		const std::string vd = std::to_string(info.vreg);
		plan.bytes = "vnb" + PCRELS(0);
		plan.locals.push_back("const int " + emul + " = (int)cpu->rvv.lmul + "
			+ std::to_string(eew) + " - (int)cpu->rvv.vsew;");
		plan.locals.push_back("const addr_t " + plan.bytes + " = (addr_t)"
			+ vector_vl_expr() + " << " + std::to_string(eew) + ";");
		plan.guard = "!cpu->rvv.vill && " + emul + " <= 3 && " + emul + " >= -3"
			" && (" + emul + " <= 0 || ((" + vd + " & ((1 << " + emul + ") - 1)) == 0"
			" && " + vd + " + (1 << " + emul + ") <= 32))"
			" && " + plan.bytes + " <= ((addr_t)" + std::to_string(lane_size)
			+ " << (" + emul + " > 0 ? " + emul + " : 0))";
		return true;
	}
	// Open the scaffolding an inlined transfer shares: the base address in a
	// local, then one test covering both the vtype guard and the arena
	// bounds, so that everything left over reaches the handler in one place.
	// Returns whether that test was emitted, and so whether there is an else.
	bool emit_vector_memory_prologue(const VectorMemInfo& info,
		const std::string& addr, const std::vector<std::string>& locals,
		const std::string& guard)
	{
		this->load_register(int(info.rs1));
		add_code("{ const addr_t " + addr + " = " + from_untracked_reg(int(info.rs1)) + ";");
		for (const auto& local : locals)
			add_code("  " + local);

		std::string cond = guard;
		if (this->vector_memory_needs_bounds_check()) {
			// The longest run still fits within the arena over-allocation, so
			// a readable/writable start address covers all of it.
			static_assert(vector_max_transfer() <= Memory<W>::OVERALLOCATE,
				"A vector register group must fit within the arena over-allocation");
			const std::string bounds =
				std::string(info.is_store ? "ARENA_WRITABLE(" : "ARENA_READABLE(") + addr + ")";
			cond = cond.empty() ? bounds : "(" + cond + ") && " + bounds;
		}
		if (!cond.empty())
			add_code("if (LIKELY(" + cond + ")) {");
		else
			add_code("{");
		return !cond.empty();
	}
	void emit_vector_memory_epilogue(const VectorMemInfo& info, bool has_fallback)
	{
		if (has_fallback) {
			add_code("} else {");
			// rs1 is the handler's only integer input, and it writes none.
			this->potentially_realize_register(int(info.rs1));
			this->emit_vector_handler_invoke();
		}
		add_code("}", "}");
	}
	// One VLEN-sized register, moved as a single struct assignment.
	void emit_vector_lane_move(const VectorMemInfo& info,
		const std::string& addr, unsigned index)
	{
		const uint64_t offset = uint64_t(index) * VectorLane::size();
		const std::string at = offset
			? "(" + addr + " + " + std::to_string(offset) + ")" : addr;
		const std::string lane = "*(VectorLaneBytes *)&" + from_rvvreg(int(info.vreg + index));
		const std::string mem  = "*(VectorLaneBytes *)" + arena_at(at);
		add_code(info.is_store ? "  " + mem + " = " + lane + ";"
		                       : "  " + lane + " = " + mem + ";");
	}
	// A run that is not a whole number of registers: a strip-mined loop's
	// last iteration, or a mask register's ceil(vl/8). Spelled as a byte
	// loop, which the C compilers we emit for turn back into the move it
	// should be, and which libtcc can compile at all.
	void emit_vector_byte_move(const VectorMemInfo& info,
		const std::string& addr, const std::string& count)
	{
		const std::string i   = "vbi" + PCRELS(0);
		const std::string dst = "vbd" + PCRELS(0);
		const std::string src = "vbs" + PCRELS(0);
		const std::string reg = "(uint8_t *)&" + from_rvvreg(int(info.vreg));
		const std::string mem = "(uint8_t *)" + arena_at(addr);
		add_code("  { uint8_t *" + dst + " = " + (info.is_store ? mem : reg) + ";",
			"    const uint8_t *" + src + " = " + (info.is_store ? reg : mem) + ";",
			"    addr_t " + i + ";",
			"    for (" + i + " = 0; " + i + " < " + count + "; " + i + "++)",
			"      " + dst + "[" + i + "] = " + src + "[" + i + "]; }");
	}
	// The contiguous run itself. A whole register is by far the common size,
	// and naming it as a constant is what lets it compile to one wide move;
	// a full multi-register group is the same move repeated.
	void emit_vector_contiguous_move(const VectorMemInfo& info,
		const std::string& addr, const VectorStridePlan& plan)
	{
		constexpr uint64_t lane_size = VectorLane::size();
		if (plan.bytes_known) {
			if (plan.known_bytes % lane_size == 0) {
				for (unsigned r = 0; r < plan.known_bytes / lane_size; r++)
					this->emit_vector_lane_move(info, addr, r);
			} else {
				this->emit_vector_byte_move(info, addr, plan.bytes);
			}
			return;
		}
		add_code("  if (LIKELY(" + plan.bytes + " == " + std::to_string(lane_size) + ")) {");
		this->emit_vector_lane_move(info, addr, 0);
		if (plan.dregs > 1) {
			// The whole group, for the iterations of a strip-mined loop that
			// are not the last one at LMUL > 1.
			add_code("  } else if (LIKELY(" + plan.bytes + " == "
				+ std::to_string(uint64_t(plan.dregs) * lane_size) + ")) {");
			for (unsigned r = 0; r < plan.dregs; r++)
				this->emit_vector_lane_move(info, addr, r);
		}
		add_code("  } else {");
		this->emit_vector_byte_move(info, addr, plan.bytes);
		add_code("  }");
	}
	// vle<eew>.v / vse<eew>.v: vl*EEW contiguous bytes, tail included, which
	// is one block copy at any vl and any EMUL -- the registers of a group
	// are adjacent, so a wider group only makes the run longer.
	void emit_vector_unit_stride(const VectorMemInfo& info)
	{
		VectorStridePlan plan;
		if (!this->vector_plan_unit_stride(info, plan)) {
			this->emit_vector_slowpath();
			return;
		}
		// An empty transfer touches no memory, and so cannot fault.
		if (plan.bytes_known && plan.known_bytes == 0)
			return;

		const std::string addr = "vaddr" + PCRELS(0);
		const bool fallback = this->emit_vector_memory_prologue(info, addr, plan.locals, plan.guard);
		this->emit_vector_contiguous_move(info, addr, plan);
		this->emit_vector_memory_epilogue(info, fallback);
	}
	// vle<eew>.v / vse<eew>.v under v0. The elements are still contiguous,
	// but the mask decides one at a time which of them move, so this is the
	// one inlined form that walks elements. Inactive elements are left
	// undisturbed, exactly as the handler leaves them.
	void emit_vector_masked_unit_stride(const VectorMemInfo& info)
	{
		static const char* const element_types[4] =
			{ "uint8_t", "uint16_t", "uint32_t", "uint64_t" };
		VectorStridePlan plan;
		if (!this->vector_plan_unit_stride(info, plan)) {
			this->emit_vector_slowpath();
			return;
		}
		if (plan.bytes_known && plan.known_bytes == 0)
			return;
		const std::string elems = m_vl_known
			? std::to_string(m_vl) : this->vector_vl_expr();

		const std::string addr = "vaddr" + PCRELS(0);
		const bool fallback = this->emit_vector_memory_prologue(info, addr, plan.locals, plan.guard);

		const char* const type = element_types[info.eew_log2];
		const std::string i    = "vmi" + PCRELS(0);
		const std::string dst  = "vmd" + PCRELS(0);
		const std::string src  = "vms" + PCRELS(0);
		const std::string mask = "vmm" + PCRELS(0);
		const std::string reg = "(" + std::string(type) + " *)&" + from_rvvreg(int(info.vreg));
		const std::string mem = "(" + std::string(type) + " *)" + arena_at(addr);
		add_code("  { " + std::string(type) + " *" + dst + " = " + (info.is_store ? mem : reg) + ";",
			"    const " + std::string(type) + " *" + src + " = " + (info.is_store ? reg : mem) + ";",
			// v0 always holds the mask, one bit per element, and is read as
			// the loop runs so that a destination overlapping it behaves as
			// the handler's element loop does.
			"    const uint8_t *" + mask + " = (const uint8_t *)&" + from_rvvreg(0) + ";",
			"    addr_t " + i + ";",
			"    for (" + i + " = 0; " + i + " < " + elems + "; " + i + "++)",
			"      if ((" + mask + "[" + i + " >> 3] >> (" + i + " & 7)) & 1)",
			"        " + dst + "[" + i + "] = " + src + "[" + i + "]; }");

		this->emit_vector_memory_epilogue(info, fallback);
	}
	// vl<n>re<eew>.v / vs<n>r.v: a raw copy of n whole registers that reads
	// neither vl nor vtype, leaving nothing to guard but the address.
	void emit_vector_whole_register(const VectorMemInfo& info)
	{
		const std::string addr = "vaddr" + PCRELS(0);
		const bool fallback = this->emit_vector_memory_prologue(info, addr, {}, "");
		for (unsigned r = 0; r < info.nregs; r++)
			this->emit_vector_lane_move(info, addr, r);
		this->emit_vector_memory_epilogue(info, fallback);
	}
	// vlm.v / vsm.v: ceil(vl/8) bytes of mask, always EEW=8 with EMUL=1 and
	// never masked. Only vill can stop it, and vl can never ask for more than
	// the one register a mask lives in.
	void emit_vector_mask_move(const VectorMemInfo& info)
	{
		constexpr uint64_t lane_size = VectorLane::size();
		if (m_vtype.known && m_vtype.vill) {
			this->emit_vector_slowpath();
			return;
		}
		VectorStridePlan plan;
		plan.guard = m_vtype.known ? "" : "!cpu->rvv.vill";
		if (m_vl_known) {
			plan.known_bytes = (m_vl + 7) / 8;
			plan.bytes_known = true;
			if (plan.known_bytes > lane_size) {
				this->emit_vector_slowpath();
				return;
			}
			if (plan.known_bytes == 0)
				return;
			plan.bytes = std::to_string(plan.known_bytes);
		} else {
			plan.bytes = "vnb" + PCRELS(0);
			plan.locals.push_back("const addr_t " + plan.bytes + " = ((addr_t)"
				+ vector_vl_expr() + " + 7) / 8;");
			plan.guard += (plan.guard.empty() ? "" : " && ")
				+ plan.bytes + " <= " + std::to_string(lane_size);
		}
		const std::string addr = "vaddr" + PCRELS(0);
		const bool fallback = this->emit_vector_memory_prologue(info, addr, plan.locals, plan.guard);
		this->emit_vector_contiguous_move(info, addr, plan);
		this->emit_vector_memory_epilogue(info, fallback);
	}
	// vlse<eew>.v / vsse<eew>.v: element *i* at base + i * x[rs2]. The run
	// is not contiguous, so this is a loop rather than a block copy, and it
	// is inlined only while it stays inside a single register (EMUL <= 1,
	// vl elements at EEW). Addresses are checked in a pass of their own:
	// a store that faulted halfway would have written elements the handler
	// then writes again.
	void emit_vector_strided(const VectorMemInfo& info)
	{
		constexpr uint64_t lane_size = VectorLane::size();
		const unsigned eew = info.eew_log2;
		const uint64_t max_elements = lane_size >> eew;
		std::string guard;
		if (m_vtype.known) {
			// EMUL = LMUL * EEW / SEW, as a register count.
			const int emul = m_vtype.lmul + int(eew) - int(m_vtype.vsew);
			if (m_vtype.vill || emul > 0 || emul < -3) {
				this->emit_vector_slowpath();
				return;
			}
			if (m_vl_known && m_vl > max_elements) {
				this->emit_vector_slowpath();
				return;
			}
			if (!m_vl_known)
				guard = vector_vl_expr() + " <= " + std::to_string(max_elements);
		} else {
			const std::string emul = "vem" + PCRELS(0);
			add_code("{ const int " + emul + " = (int)cpu->rvv.lmul + "
				+ std::to_string(eew) + " - (int)cpu->rvv.vsew;");
			guard = "!cpu->rvv.vill && " + emul + " <= 0 && " + emul + " >= -3 && "
				+ vector_vl_expr() + " <= " + std::to_string(max_elements);
		}
		if (m_vtype.known)
			add_code("{");

		const std::string base = "vsa" + PCRELS(0);
		const std::string step = "vss" + PCRELS(0);
		const std::string n    = "vsn" + PCRELS(0);
		const std::string at   = "vsp" + PCRELS(0);
		const std::string i    = "vsi" + PCRELS(0);
		this->load_register(int(info.rs1));
		this->load_register(int(info.rs2));
		add_code("  const addr_t " + base + " = " + from_untracked_reg(int(info.rs1)) + ";",
			"  const addr_t " + step + " = " + from_untracked_reg(int(info.rs2)) + ";",
			"  const addr_t " + n + " = (addr_t)" + vector_vl_expr() + ";");
		if (!guard.empty())
			add_code("if (LIKELY(" + guard + ")) {");
		else
			add_code("{");

		add_code("  addr_t " + i + "; addr_t " + at + ";");
		bool checked = this->vector_memory_needs_bounds_check();
		if (checked) {
			const std::string ok = "vsok" + PCRELS(0);
			const char* const test = info.is_store ? "ARENA_WRITABLE(" : "ARENA_READABLE(";
			add_code("  int " + ok + "; " + ok + " = 1;",
				"  for (" + i + " = 0, " + at + " = " + base + "; " + i + " < " + n
					+ "; " + i + "++, " + at + " += " + step + ")",
				"    " + ok + " &= " + test + at + ");",
				"if (LIKELY(" + ok + ")) {");
		}
		const std::string type = vector_int_utype(eew);
		const std::string reg = from_rvvreg(int(info.vreg)) + "."
			+ vector_int_field(eew) + "[" + i + "]";
		const std::string mem = "*(" + type + " *)" + arena_at(at);
		add_code("  for (" + i + " = 0, " + at + " = " + base + "; " + i + " < " + n
				+ "; " + i + "++, " + at + " += " + step + ")",
			"    " + (info.is_store ? mem + " = " + reg : reg + " = " + mem) + ";");
		if (checked) {
			add_code("} else {");
			this->potentially_realize_register(int(info.rs1));
			this->potentially_realize_register(int(info.rs2));
			this->emit_vector_handler_invoke();
			add_code("}");
		}
		if (!guard.empty()) {
			add_code("} else {");
			this->potentially_realize_register(int(info.rs1));
			this->potentially_realize_register(int(info.rs2));
			this->emit_vector_handler_invoke();
		}
		add_code("}", "}");
	}
	// vlxei<eew>.v / vsxei<eew>.v: element *i* at base + vs2[i], with the
	// encoding giving the width of the *index* and vtype the width of the
	// data. That makes the body SEW-dependent, so it is emitted per SEW
	// behind the same guard the integer arithmetic uses -- which already
	// pins LMUL=1 and vl to one register, the only shape inlined here.
	// Ordered and unordered forms are both emitted: the loop runs in index
	// order, which is what the ordered form asks for and the unordered one
	// permits.
	void emit_vector_indexed_body(const VectorMemInfo& info, unsigned vsew)
	{
		const unsigned ieew = info.eew_log2;
		const std::string sfx = PCRELS(0) + "_" + std::to_string(vsew);
		const std::string base = "vxa" + sfx;
		const std::string n    = "vxn" + sfx;
		const std::string i    = "vxi" + sfx;
		const std::string at   = "vxp" + sfx;
		add_code("  {");
		this->load_register(int(info.rs1));
		add_code("  const addr_t " + base + " = " + from_untracked_reg(int(info.rs1)) + ";",
			"  const addr_t " + n + " = (addr_t)" + vector_vl_expr() + ";",
			"  addr_t " + i + "; addr_t " + at + ";");
		const std::string index = base + " + (addr_t)" + from_rvvreg(int(info.vs2))
			+ "." + vector_int_field(ieew) + "[" + i + "]";
		const bool checked = this->vector_memory_needs_bounds_check();
		if (checked) {
			const std::string ok = "vxok" + sfx;
			const char* const test = info.is_store ? "ARENA_WRITABLE(" : "ARENA_READABLE(";
			add_code("  int " + ok + "; " + ok + " = 1;",
				"  for (" + i + " = 0; " + i + " < " + n + "; " + i + "++) {",
				"    " + at + " = " + index + ";",
				"    " + ok + " &= " + test + at + "); }",
				"  if (LIKELY(" + ok + ")) {");
		}
		const std::string type = vector_int_utype(vsew);
		const std::string reg = from_rvvreg(int(info.vreg)) + "."
			+ vector_int_field(vsew) + "[" + i + "]";
		const std::string mem = "*(" + type + " *)" + arena_at(at);
		add_code("  for (" + i + " = 0; " + i + " < " + n + "; " + i + "++) {",
			"    " + at + " = " + index + ";",
			"    " + (info.is_store ? mem + " = " + reg : reg + " = " + mem) + "; }");
		if (checked) {
			add_code("  } else {");
			this->potentially_realize_register(int(info.rs1));
			this->emit_vector_handler_invoke();
			add_code("  }");
		}
		add_code("  }");
	}
	void emit_vector_indexed(const VectorMemInfo& info)
	{
		// The index group is sized by the index width against SEW, so a SEW
		// narrower than the index would need several index registers.
		std::vector<unsigned> sews;
		for (const unsigned vsew : this->vector_int_sews()) {
			// EMUL of the index group is LMUL * index-EEW / SEW, and the
			// guard already holds LMUL <= 0, so it can only be smaller than
			// this -- but it still has to stay above the fractional limit.
			if (int(info.eew_log2) - int(vsew) <= 0)
				sews.push_back(vsew);
		}
		if (sews.empty()) {
			this->emit_vector_slowpath();
			return;
		}
		for (const unsigned vsew : sews)
			(void)this->rvv_int_guard(vsew);

		for (const unsigned vsew : sews) {
			// LMUL + log2(EEW/SEW) >= -3, spelled as a bound on LMUL.
			const int lmul_min = int(vsew) - int(info.eew_log2) - 3;
			std::string legal;
			if (lmul_min > -3) {
				legal = m_vtype.known
					? (m_vtype.lmul >= lmul_min ? "" : "0")
					: " && cpu->rvv.lmul >= " + std::to_string(lmul_min);
				if (legal == "0") {
					add_code("if (0) {");
					add_code("} else {");
					continue;
				}
			}
			add_code("if (LIKELY(" + rvv_int_guard(vsew) + legal + ")) {");
			this->emit_vector_indexed_body(info, vsew);
			add_code("} else {");
		}
		this->emit_vector_slowpath();
		for (size_t i = 0; i < sews.size(); i++)
			add_code("}");
	}
	// vlseg<nf>e<eew>.v / vsseg<nf>e<eew>.v: nf fields interleaved in
	// memory, one register each, de-interleaved on the way in. The whole
	// run is contiguous, so one bounds check covers it -- but the elements
	// are not, so it is a loop with the field count unrolled into it.
	void emit_vector_segment(const VectorMemInfo& info)
	{
		constexpr uint64_t lane_size = VectorLane::size();
		const unsigned eew = info.eew_log2;
		const unsigned nf = info.nregs;
		const uint64_t max_elements = lane_size >> eew;
		std::string guard;
		if (m_vtype.known) {
			// EMUL > 1 would give each field a register group, which this
			// body does not walk.
			const int emul = m_vtype.lmul + int(eew) - int(m_vtype.vsew);
			if (m_vtype.vill || emul > 0 || emul < -3) {
				this->emit_vector_slowpath();
				return;
			}
			if (m_vl_known && m_vl > max_elements) {
				this->emit_vector_slowpath();
				return;
			}
			if (!m_vl_known)
				guard = vector_vl_expr() + " <= " + std::to_string(max_elements);
			add_code("{");
		} else {
			const std::string emul = "vem" + PCRELS(0);
			add_code("{ const int " + emul + " = (int)cpu->rvv.lmul + "
				+ std::to_string(eew) + " - (int)cpu->rvv.vsew;");
			guard = "!cpu->rvv.vill && " + emul + " <= 0 && " + emul + " >= -3 && "
				+ vector_vl_expr() + " <= " + std::to_string(max_elements);
		}
		const std::string base = "vga" + PCRELS(0);
		const std::string n    = "vgn" + PCRELS(0);
		const std::string i    = "vgi" + PCRELS(0);
		this->load_register(int(info.rs1));
		add_code("  const addr_t " + base + " = " + from_untracked_reg(int(info.rs1)) + ";",
			"  const addr_t " + n + " = (addr_t)" + vector_vl_expr() + ";");
		if (this->vector_memory_needs_bounds_check()) {
			// nf * EMUL is at most 8 registers, so the run still fits the
			// arena over-allocation and the base address covers all of it.
			static_assert(vector_max_transfer() <= Memory<W>::OVERALLOCATE,
				"A segmented transfer must fit within the arena over-allocation");
			const std::string bounds =
				std::string(info.is_store ? "ARENA_WRITABLE(" : "ARENA_READABLE(") + base + ")";
			guard = guard.empty() ? bounds : "(" + guard + ") && " + bounds;
		}
		if (!guard.empty())
			add_code("if (LIKELY(" + guard + ")) {");
		else
			add_code("{");

		const std::string type = vector_int_utype(eew);
		const std::string stride = std::to_string(uint64_t(nf) << eew);
		add_code("  addr_t " + i + ";",
			"  for (" + i + " = 0; " + i + " < " + n + "; " + i + "++) {");
		for (unsigned f = 0; f < nf; f++) {
			const std::string at = base + " + " + i + " * " + stride
				+ (f ? " + " + std::to_string(uint64_t(f) << eew) : "");
			const std::string mem = "*(" + type + " *)" + arena_at("(" + at + ")");
			const std::string reg = from_rvvelem(info.vreg + f, eew, i);
			add_code("    " + (info.is_store ? mem + " = " + reg : reg + " = " + mem) + ";");
		}
		add_code("  }");
		if (!guard.empty()) {
			add_code("} else {");
			this->potentially_realize_register(int(info.rs1));
			this->emit_vector_handler_invoke();
		}
		add_code("}", "}");
	}
	// VMV.X.S: x[rd] = sext(vs2[0]). The one OPMVV code point worth
	// inlining on its own -- a compiler reaches for it whenever it takes a
	// single element back out of a vector, so it shows up in the scalar
	// tail of any vectorized loop.
	static bool vector_is_move_xs(const rv32i_instruction& vinstr) noexcept
	{
		if (vinstr.opcode() != RV32V_OP || vinstr.vwidth() != 0x2)
			return false; // Not OPM.VV
		const rv32v_instruction vi { vinstr };
		// vs1 == 0 picks VMV.X.S out of VWXUNARY0; x0 as the destination
		// would be a write to a register that does not keep it.
		return vi.OPVV.funct6 == 0b010000 && vi.OPVV.vs1 == 0 && vi.OPVV.vd != 0;
	}
	void emit_vector_move_xs()
	{
		const rv32v_instruction vi { instr };
		const auto sews = this->vector_int_sews();
		if (sews.empty()) {
			this->emit_vector_slowpath();
			return;
		}
		for (const unsigned vsew : sews)
			(void)this->rvv_int_guard(vsew);

		this->reset_tracked_register(int(vi.OPVV.vd));
		for (const unsigned vsew : sews) {
			add_code("if (LIKELY(" + rvv_int_guard(vsew) + ")) {");
			add_code("  " + to_reg(int(vi.OPVV.vd)) + " = (addr_t)("
				+ vector_int_stype(vsew) + ")"
				+ from_rvvelem(vi.OPVV.vs2, vsew, "0") + ";");
			add_code("} else {");
		}
		this->emit_vector_slowpath();
		for (size_t i = 0; i < sews.size(); i++)
			add_code("}");
	}
	// Emit one vector load or store that does not need the interpreter.
	void emit_vector_memory(const VectorMemInfo& info)
	{
		switch (info.form) {
		case VectorMemForm::UnitStride:       this->emit_vector_unit_stride(info); break;
		case VectorMemForm::Strided:          this->emit_vector_strided(info); break;
		case VectorMemForm::Indexed:          this->emit_vector_indexed(info); break;
		case VectorMemForm::Segment:          this->emit_vector_segment(info); break;
		case VectorMemForm::MaskedUnitStride: this->emit_vector_masked_unit_stride(info); break;
		case VectorMemForm::WholeRegister:    this->emit_vector_whole_register(info); break;
		case VectorMemForm::Mask:             this->emit_vector_mask_move(info); break;
		default:                              this->emit_vector_slowpath(); break;
		}
	}
	// One fused multiply-add element, exactly as the OPFVV/OPFVF handlers
	// compute it: a is vs1 (or the scalar), b is vs2, c is the old value of
	// the destination. The product is unrounded, hence the api.fma call.
	static std::string vector_fma_expression(unsigned funct6, const std::string& fma,
		const std::string& a, const std::string& b, const std::string& c)
	{
		const std::string call = fma + "(";
		switch (funct6) {
		case 0b101000: return       call +       a + ", " + c + ", " + b + ")"; // VFMADD
		case 0b101001: return "-" + call +       a + ", " + c + ", " + b + ")"; // VFNMADD
		case 0b101010: return       call +       a + ", " + c + ", -" + b + ")"; // VFMSUB
		case 0b101011: return       call + "-" + a + ", " + c + ", " + b + ")"; // VFNMSUB
		case 0b101100: return       call +       a + ", " + b + ", " + c + ")"; // VFMACC
		case 0b101101: return "-" + call +       a + ", " + b + ", " + c + ")"; // VFNMACC
		case 0b101110: return       call +       a + ", " + b + ", -" + c + ")"; // VFMSAC
		default:       return       call + "-" + a + ", " + b + ", " + c + ")"; // VFNMSAC
		}
	}
	// Inline float arithmetic as straight-line code. The vl/vtype guard
	// pins a full m1 register at this SEW: every element active, no tail.
	void emit_vector_float_arith_body(unsigned vsew)
	{
		const rv32v_instruction vi { instr };
		const bool dbl = (vsew == 3);
		const bool is_scalar = instr.vwidth() == 0x5; // OPF.VF takes f[rs1]
		const char* const elem = dbl ? ".f64[" : ".f32[";
		std::string rhs;
		if (is_scalar) {
			// The guard pins SEW, so the scalar is the low element.
			rhs = "vscalar" + PCRELS(0) + "_" + std::to_string(vsew);
			add_code(std::string("  const ") + (dbl ? "double " : "float ") + rhs + " = "
				+ from_fpreg(vi.OPVV.vs1) + (dbl ? ".f64;" : ".f32[0];"));
		}
		const char* const op = vector_float_operator(instr);
		const std::string fma = dbl ? "api.vfmaf64" : "api.vfmaf32";
		const unsigned elements = VectorLane::size() >> vsew;
		for (unsigned i = 0; i < elements; i++) {
			const std::string e = elem + std::to_string(i) + "]";
			const std::string a = is_scalar ? rhs : from_rvvreg(vi.OPVV.vs1) + e;
			const std::string b = from_rvvreg(vi.OPVV.vs2) + e;
			const std::string d = from_rvvreg(vi.OPVV.vd) + e;
			add_code("  " + d + " = " + (op != nullptr
				? b + op + a
				: vector_fma_expression(vi.OPVV.funct6, fma, a, b, d)) + ";");
		}
	}
	// ── Element-wise integer arithmetic ─────────────────────────────────
	// The OPIVV/OPIVX/OPIVI code points that are one C expression per
	// element. Everything else -- the mask-producing compares, the
	// widening/narrowing forms, gathers, slides and reductions -- stays
	// with the interpreter handler.
	enum class VectorIntOp {
		None, Add, Sub, Rsub, And, Or, Xor, Andn,
		Minu, Min, Maxu, Max, Sll, Srl, Sra, Ror, Rol, Move,
		Slideup, Slidedown, // the element index moves, the value does not
		Zext, Sext          // ... and the element grows on the way
	};
	struct VectorIntInfo {
		VectorIntOp op = VectorIntOp::None;
		unsigned vd  = 0;
		unsigned vs1 = 0; // vs1, or x[rs1] in the .vx form
		unsigned vs2 = 0;
		unsigned form = 0;    // funct3: 0 = .vv, 3 = .vi, 4 = .vx
		uint64_t imm  = 0;    // .vi: the immediate, already extended
		bool grouped  = false; // the body walks a multi-register group
		bool is_vv() const noexcept { return form == 0x0; }
		bool is_vi() const noexcept { return form == 0x3; }
		bool is_vx() const noexcept { return form == 0x4; }
	};
	// Classify one OPI encoding. Only unmasked forms qualify: a mask makes
	// element activity a runtime property of v0, which the straight-line
	// body below has no cheap way to honour.
	static VectorIntInfo vector_int_form(const rv32i_instruction& vinstr)
	{
		if (vinstr.opcode() != RV32V_OP)
			return {};
		const unsigned width = vinstr.vwidth();
		const rv32v_instruction vi0 { vinstr };
		if (width == 0x2 && vi0.OPVV.vm && vi0.OPVV.funct6 == 0b010010) {
			// VXUNARY0: the integer extensions. The vs1 field selects the
			// factor (8, 4 or 2) and its low bit the signedness, as
			// int_extension() (rvv_instr.cpp) decodes it.
			const unsigned sel = vi0.OPVV.vs1;
			if (sel < 0b00010 || sel > 0b00111)
				return {};
			VectorIntInfo ext;
			ext.form = width;
			ext.vd  = vi0.OPVV.vd;
			ext.vs2 = vi0.OPVV.vs2;
			ext.imm = (sel <= 3) ? 3 : (sel <= 5) ? 2 : 1; // log2 of the factor
			ext.op = (sel & 1) ? VectorIntOp::Sext : VectorIntOp::Zext;
			return ext;
		}
		if (width != 0x0 && width != 0x3 && width != 0x4)
			return {}; // Not OPI.VV / OPI.VI / OPI.VX
		const rv32v_instruction vi { vinstr };
		if (!vi.OPVV.vm)
			return {};
		VectorIntInfo info;
		info.form = width;
		info.vd  = vi.OPVV.vd;
		info.vs1 = vi.OPVV.vs1;
		info.vs2 = vi.OPVV.vs2;
		// Sign-extended for the arithmetic and bitwise forms, zero-extended
		// for the shifts: the same split the OPIVI handler makes.
		const uint32_t imm5 = vi.OPVI.imm;
		info.imm = uint64_t(int64_t(imm5 ^ 0x10) - 0x10);
		switch (vi.OPVV.funct6) {
		case 0b000000: info.op = VectorIntOp::Add; break;
		case 0b000001: // VANDN (Zvbb), no .vi form
			if (info.is_vi()) return {};
			info.op = VectorIntOp::Andn; break;
		case 0b000010: // VSUB, no .vi form (VRSUB covers it)
			if (info.is_vi()) return {};
			info.op = VectorIntOp::Sub; break;
		case 0b000011: // VRSUB: .vx / .vi only
			if (info.is_vv()) return {};
			info.op = VectorIntOp::Rsub; break;
		case 0b000100: case 0b000101: // VMINU / VMIN
		case 0b000110: case 0b000111: // VMAXU / VMAX
			if (info.is_vi()) return {};
			switch (vi.OPVV.funct6) {
			case 0b000100: info.op = VectorIntOp::Minu; break;
			case 0b000101: info.op = VectorIntOp::Min;  break;
			case 0b000110: info.op = VectorIntOp::Maxu; break;
			default:       info.op = VectorIntOp::Max;  break;
			}
			break;
		case 0b001001: info.op = VectorIntOp::And; break;
		case 0b001010: info.op = VectorIntOp::Or;  break;
		case 0b001011: info.op = VectorIntOp::Xor; break;
		case 0b010100: // VROR.VV/VX, or VROR.VI with the high bit clear
		case 0b010101: // VROL.VV/VX, or VROR.VI with it set
			// The .vi rotate needs six bits of shift amount and borrows the
			// low bit of funct6 for the sixth, so it is always a rotate
			// right; the .vx and .vv forms use that bit to pick a direction.
			if (info.is_vi()) {
				info.op = VectorIntOp::Ror;
				info.imm = ((vi.OPVV.funct6 & 1) << 5) | imm5;
			} else {
				info.op = (vi.OPVV.funct6 == 0b010100)
					? VectorIntOp::Ror : VectorIntOp::Rol;
			}
			break;
		case 0b001110: // VSLIDEUP.VI / .VX (there is no .vv form)
		case 0b001111: // VSLIDEDOWN.VI / .VX
			if (info.is_vv()) return {};
			info.op = (vi.OPVV.funct6 == 0b001110)
				? VectorIntOp::Slideup : VectorIntOp::Slidedown;
			info.imm = imm5; // The slide offset is zero-extended
			break;
		case 0b010111: // VMV.V.V / VMV.V.X / VMV.V.I (the vm=0 form is VMERGE)
			info.op = VectorIntOp::Move; break;
		case 0b100101: info.op = VectorIntOp::Sll; info.imm = imm5; break;
		case 0b101000: info.op = VectorIntOp::Srl; info.imm = imm5; break;
		case 0b101001: info.op = VectorIntOp::Sra; info.imm = imm5; break;
		default: return {};
		}
		return info;
	}
	// The SEWs an integer body is emitted for. Unlike the float path this
	// is every width, so a block that has not proven its vtype pays a
	// guarded body per SEW -- only one of which can be reached at runtime.
	std::vector<unsigned> vector_int_sews() const
	{
		if (!m_vtype.known)
			return { 0, 1, 2, 3 };
		if (m_vtype.vill)
			return {};
		return { m_vtype.vsew };
	}
	// Not every operation exists at every SEW: an extension needs a source
	// element that is still at least a byte wide.
	static bool vector_int_sew_ok(const VectorIntInfo& info, unsigned vsew) noexcept
	{
		if (info.op == VectorIntOp::Zext || info.op == VectorIntOp::Sext)
			return vsew >= unsigned(info.imm);
		return true;
	}
	// The guard for an integer body: this SEW, at most one register, a
	// valid vtype. vl is deliberately *not* pinned -- the body loops to vl,
	// which is what the handler does, tail elements included (they stay
	// put). A fractional LMUL passes too: it holds fewer elements, but they
	// sit at the same offsets in the same single register.
	const std::string& rvv_int_guard(unsigned vsew)
	{
		auto& guard = m_rvv_int_guard.at(vsew);
		if (guard.empty()) {
			if (m_vtype.known) {
				guard = (!m_vtype.vill && m_vtype.lmul <= 0
					&& m_vtype.vsew == vsew) ? "1" : "0";
			} else {
				guard = "viok" + PCRELS(0) + "_" + std::to_string(vsew);
				// Declared and assigned separately, as in rvv_guard().
				add_code("int " + guard + "; " + guard + " = cpu->rvv.vsew == "
					+ std::to_string(vsew) + " && cpu->rvv.lmul <= 0"
					" && !cpu->rvv.vill && " + vector_vl_expr() + " <= "
					+ std::to_string(rvv_full_vl(vsew)) + ";");
			}
		}
		return guard;
	}
	// The guard for a body that walks a multi-register group: a valid
	// vtype at this SEW, an LMUL above one, vl inside the group, and every
	// register the instruction names aligned to the group and inside the
	// file -- the alignment the handler would otherwise trap on. Returns an
	// empty string when no such body can be reached from here.
	//
	// Unlike rvv_int_guard() this is spelled out in place rather than kept
	// in a block-level local: a group is the rarer shape, and a local would
	// put its whole computation on the path of every block that has one.
	std::string rvv_int_group_guard(const VectorIntInfo& info, unsigned vsew)
	{
		unsigned regmask = info.vd | info.vs2;
		unsigned regmax  = std::max(info.vd, info.vs2);
		if (info.is_vv()) {
			regmask |= info.vs1;
			regmax   = std::max(regmax, info.vs1);
		}
		if (m_vtype.known) {
			const unsigned dregs = 1u << m_vtype.lmul;
			if ((regmask & (dregs - 1)) != 0 || regmax + dregs > 32)
				return {}; // The handler traps on it, so it has to go there
			const uint64_t max_vl = uint64_t(rvv_full_vl(vsew)) << m_vtype.lmul;
			if (m_vl_known)
				return (m_vl <= max_vl) ? "1" : "";
			return vector_vl_expr() + " <= " + std::to_string(max_vl);
		}
		const std::string dregs = "(1u << cpu->rvv.lmul)";
		return "cpu->rvv.vsew == " + std::to_string(vsew)
			+ " && cpu->rvv.lmul > 0 && cpu->rvv.lmul <= 3 && !cpu->rvv.vill && "
			+ vector_vl_expr() + " <= ((addr_t)" + std::to_string(rvv_full_vl(vsew))
			+ " << cpu->rvv.lmul) && (" + std::to_string(regmask) + "u & ("
			+ dregs + " - 1)) == 0 && " + std::to_string(regmax) + "u + " + dregs + " <= 32";
	}
	// Whether this operation means the same thing across a group. The
	// slides and the extensions do not: their element index and source
	// width are relative to VLMAX and to EMUL, both of which the group
	// changes.
	static bool vector_int_op_groupable(const VectorIntInfo& info) noexcept
	{
		switch (info.op) {
		case VectorIntOp::Slideup:
		case VectorIntOp::Slidedown:
		case VectorIntOp::Zext:
		case VectorIntOp::Sext:
			return false;
		default:
			return true;
		}
	}
	static const char* vector_int_field(unsigned vsew) noexcept {
		switch (vsew) {
		case 0:  return "u8";
		case 1:  return "u16";
		case 2:  return "u32";
		default: return "u64";
		}
	}
	static const char* vector_int_stype(unsigned vsew) noexcept {
		switch (vsew) {
		case 0:  return "int8_t";
		case 1:  return "int16_t";
		case 2:  return "int32_t";
		default: return "int64_t";
		}
	}
	static const char* vector_int_utype(unsigned vsew) noexcept {
		switch (vsew) {
		case 0:  return "uint8_t";
		case 1:  return "uint16_t";
		case 2:  return "uint32_t";
		default: return "uint64_t";
		}
	}
	// Element *idx* of a vector register, at this SEW.
	std::string from_rvvelem(unsigned reg, unsigned vsew, const std::string& idx) {
		return from_rvvreg(int(reg)) + "." + vector_int_field(vsew) + "[" + idx + "]";
	}
	// Element *idx* of a register *group* starting at reg: the registers of
	// a group are adjacent, so this is element_at() (rvv_instr.cpp) spelled
	// in C -- one flat array over the whole register file. Only the guarded
	// bodies use it, and their guard has already put every index they can
	// reach inside the group.
	std::string from_rvvgroup(unsigned reg, unsigned vsew, const std::string& idx) {
		return "((" + std::string(vector_int_utype(vsew)) + " *)&"
			+ from_rvvreg(0) + ")[" + std::to_string(reg * rvv_full_vl(vsew))
			+ " + " + idx + "]";
	}
	std::string vector_int_elem(const VectorIntInfo& info, unsigned reg,
		unsigned vsew, const std::string& idx) {
		return info.grouped ? from_rvvgroup(reg, vsew, idx)
		                    : from_rvvelem(reg, vsew, idx);
	}
	// One destination element, as the interpreter computes it.
	std::string vector_int_element_expr(const VectorIntInfo& info,
		unsigned vsew, const std::string& idx)
	{
		const std::string U = std::string("(") + vector_int_utype(vsew) + ")";
		const std::string S = std::string("(") + vector_int_stype(vsew) + ")";
		const unsigned bits = 8u << vsew;
		const std::string a = vector_int_elem(info, info.vs2, vsew, idx);
		std::string b;
		if (info.is_vv())
			b = vector_int_elem(info, info.vs1, vsew, idx);
		else if (info.is_vx())
			b = U + "(" + from_untracked_reg(int(info.vs1)) + ")";
		else
			b = U + std::to_string(info.imm) + "ULL";
		// Shift and rotate amounts are taken modulo SEW, as in the handlers.
		const std::string sh = "((" + b + ") & " + std::to_string(bits - 1) + ")";
		const unsigned elements = rvv_full_vl(vsew);
		if (info.op == VectorIntOp::Slideup || info.op == VectorIntOp::Slidedown) {
			// The slide offset is a whole register value, not an element of
			// this SEW, so it does not go through *b* above.
			const std::string off = info.is_vx()
				? "(addr_t)(" + from_untracked_reg(int(info.vs1)) + ")"
				: "(addr_t)" + std::to_string(info.imm);
			// The source index is masked to the register: it is only read
			// where the test that guards it has already put it in range.
			const std::string mask = " & " + std::to_string(elements - 1);
			if (info.op == VectorIntOp::Slideup) {
				// Elements below the offset keep their old value, which is
				// the same ascending in-place walk the handler makes.
				return "(" + idx + " >= " + off + ") ? "
					+ from_rvvelem(info.vs2, vsew, "((" + idx + " - " + off + ")" + mask + ")")
					+ " : " + from_rvvelem(info.vd, vsew, idx);
			}
			// Sliding down past VLMAX reads zero, not the tail -- and a
			// fractional LMUL puts VLMAX below a whole register, so the
			// bound is not the element count. The guard has already put
			// LMUL at one or below, which is what makes it a shift right.
			const std::string vlmax = m_vtype.known
				? std::to_string(elements >> unsigned(m_vtype.lmul < 0 ? -m_vtype.lmul : 0))
				: "((addr_t)" + std::to_string(elements) + " >> (unsigned)-cpu->rvv.lmul)";
			return "(" + idx + " + " + off + " < " + vlmax + ") ? "
				+ from_rvvelem(info.vs2, vsew, "((" + idx + " + " + off + ")" + mask + ")")
				+ " : 0";
		}
		if (info.op == VectorIntOp::Zext || info.op == VectorIntOp::Sext) {
			const unsigned nsew = vsew - unsigned(info.imm);
			const std::string src = from_rvvelem(info.vs2, nsew, idx);
			return info.op == VectorIntOp::Sext
				? U + "(" + vector_int_stype(nsew) + ")" + src
				: U + src;
		}
		switch (info.op) {
		case VectorIntOp::Add:  return U + "((" + a + ") + (" + b + "))";
		case VectorIntOp::Sub:  return U + "((" + a + ") - (" + b + "))";
		case VectorIntOp::Rsub: return U + "((" + b + ") - (" + a + "))";
		case VectorIntOp::And:  return U + "((" + a + ") & (" + b + "))";
		case VectorIntOp::Or:   return U + "((" + a + ") | (" + b + "))";
		case VectorIntOp::Xor:  return U + "((" + a + ") ^ (" + b + "))";
		case VectorIntOp::Andn: return U + "((" + a + ") & ~(" + b + "))";
		case VectorIntOp::Minu: return U + "((" + a + ") < (" + b + ") ? (" + a + ") : (" + b + "))";
		case VectorIntOp::Maxu: return U + "((" + a + ") > (" + b + ") ? (" + a + ") : (" + b + "))";
		case VectorIntOp::Min:  return U + "(" + S + "(" + a + ") < " + S + "(" + b + ") ? " + S + "(" + a + ") : " + S + "(" + b + "))";
		case VectorIntOp::Max:  return U + "(" + S + "(" + a + ") > " + S + "(" + b + ") ? " + S + "(" + a + ") : " + S + "(" + b + "))";
		case VectorIntOp::Sll:  return U + "((" + a + ") << " + sh + ")";
		case VectorIntOp::Srl:  return U + "((" + a + ") >> " + sh + ")";
		case VectorIntOp::Sra:  return U + "(" + S + "(" + a + ") >> " + sh + ")";
		// A rotate by zero would shift by the full width, which is undefined
		// in C, so the counter-shift is taken modulo SEW as well.
		case VectorIntOp::Ror:  return U + "(((" + a + ") >> " + sh + ") | " + U + "((" + a
			+ ") << ((" + std::to_string(bits) + " - " + sh + ") & " + std::to_string(bits - 1) + ")))";
		case VectorIntOp::Rol:  return U + "(" + U + "((" + a + ") << " + sh + ") | ((" + a
			+ ") >> ((" + std::to_string(bits) + " - " + sh + ") & " + std::to_string(bits - 1) + ")))";
		default:                return b; // VMV.V.*
		}
	}
	void emit_vector_int_loop(const VectorIntInfo& info, unsigned vsew,
		const std::string& idx, const std::string& bound)
	{
		add_code("    for (" + idx + " = 0; " + idx + " < " + bound + "; " + idx + "++)",
			"      " + vector_int_elem(info, info.vd, vsew, idx) + " = "
			+ vector_int_element_expr(info, vsew, idx) + ";");
	}
	// The trip count is what decides whether the host compiler can turn a
	// register's worth of elements into one SIMD instruction, so it is
	// spelled as a constant wherever it is known. The two worth testing for
	// at runtime are a full register and half of one -- a vector loop
	// written against the 128-bit minimum VLEN runs at half of our 256-bit
	// register -- and anything else is strip-mined a register at a time,
	// which is also what carries a multi-register group.
	void emit_vector_int_arith_body(const VectorIntInfo& info, unsigned vsew)
	{
		const unsigned elements = rvv_full_vl(vsew);
		const std::string sfx = PCRELS(0) + "_" + std::to_string(vsew)
			+ (info.grouped ? "g" : "");
		const std::string idx = "vie" + sfx;
		add_code("  {", "  addr_t " + idx + ";");
		if (m_vl_known && !info.grouped) {
			this->emit_vector_int_loop(info, vsew, idx,
				std::to_string(std::min<uint64_t>(m_vl, elements)));
			add_code("  }");
			return;
		}
		const std::string vn = "vin" + sfx;
		add_code("  const addr_t " + vn + " = (addr_t)" + vector_vl_expr() + ";");
		if (!info.grouped) {
			for (unsigned n = elements; n * 2 >= elements; n /= 2) {
				add_code(std::string("  ") + (n == elements ? "if" : "else if")
					+ " (LIKELY(" + vn + " == " + std::to_string(n) + ")) {");
				this->emit_vector_int_loop(info, vsew, idx, std::to_string(n));
				add_code("  }");
			}
			add_code("  else {");
			this->emit_vector_int_loop(info, vsew, idx, vn);
			add_code("  }");
			add_code("  }");
			return;
		}
		// A group: whole registers first, in a loop whose inner trip count
		// is a constant and so still vectorizes, then whatever is left.
		const std::string k = "vik" + sfx;
		const std::string body_idx = "(" + idx + " + " + k + ")";
		add_code("  addr_t " + k + ";",
			"  for (" + idx + " = 0; " + idx + " + " + std::to_string(elements)
				+ " <= " + vn + "; " + idx + " += " + std::to_string(elements) + ")",
			"    for (" + k + " = 0; " + k + " < " + std::to_string(elements) + "; " + k + "++)",
			"      " + vector_int_elem(info, info.vd, vsew, body_idx) + " = "
				+ vector_int_element_expr(info, vsew, body_idx) + ";");
		add_code("  for (; " + idx + " < " + vn + "; " + idx + "++)",
			"    " + vector_int_elem(info, info.vd, vsew, idx) + " = "
				+ vector_int_element_expr(info, vsew, idx) + ";");
		add_code("  }");
	}
	// Emit one integer operation: a guarded body per candidate SEW, with
	// the handler as the last resort.
	void emit_vector_int_arith(const VectorIntInfo& info)
	{
		std::vector<unsigned> sews;
		for (const unsigned vsew : this->vector_int_sews())
			if (vector_int_sew_ok(info, vsew))
				sews.push_back(vsew);
		if (sews.empty()) {
			this->emit_vector_slowpath();
			return;
		}
		// Every guard is materialized first, for the reason rvv_guard()
		// gives: they are C locals, and the bodies below open scopes a
		// later declaration could not escape.
		struct IntBody { std::string guard; unsigned vsew; bool grouped; };
		std::vector<IntBody> bodies;
		// Single-register bodies first: their guard is a live local, so the
		// shape a program almost always has is reached after a compare or
		// two, and the group guards -- which are spelled out in full -- are
		// only ever evaluated once those have failed.
		const bool any_single = !m_vtype.known || m_vtype.lmul <= 0;
		const bool any_group  = !m_vtype.known || m_vtype.lmul > 0;
		if (any_single) {
			for (const unsigned vsew : sews)
				bodies.push_back({ this->rvv_int_guard(vsew), vsew, false });
		}
		if (vector_int_op_groupable(info) && any_group) {
			for (const unsigned vsew : sews) {
				std::string guard = this->rvv_int_group_guard(info, vsew);
				if (!guard.empty())
					bodies.push_back({ std::move(guard), vsew, true });
			}
		}
		if (bodies.empty()) {
			this->emit_vector_slowpath();
			return;
		}
		for (const auto& body : bodies) {
			VectorIntInfo shaped = info;
			shaped.grouped = body.grouped;
			add_code("if (LIKELY(" + body.guard + ")) {");
			this->emit_vector_int_arith_body(shaped, body.vsew);
			add_code("} else {");
		}
		this->emit_vector_slowpath();
		for (size_t i = 0; i < bodies.size(); i++)
			add_code("}");
	}
	// True for the two configuration forms with an immediate vtype, which is
	// what compilers emit. VSETVL takes it from a register instead, so its
	// vtype is only known at runtime.
	bool vector_is_inlinable_setvl(const rv32i_instruction& vinstr) const noexcept
	{
		if (vinstr.opcode() != RV32V_OP || vinstr.vwidth() != 0x7)
			return false;
		const auto func = vinstr.vsetfunc();
		return func == 0x0 || func == 0x1 || func == 0x3;
	}
	// Inline vsetvli/vsetivli. Strip-mined loops execute one per iteration,
	// and as a handler call it would realize and reload the cached integer
	// registers every time, which is most of what it costs. Inlined it is
	// a compare and a few stores, and the block gets a known vtype.
	void emit_vector_setvl()
	{
		const rv32v_instruction vi { instr };
		const bool imm_avl = instr.vsetfunc() == 0x3; // VSETIVLI
		// vsetivli's vtype is ten bits. The two above it mark the form.
		const uint32_t vtypei = imm_avl ? (vi.IVLI.zimm & 0x3FF) : vi.VLI.zimm;
		const int rd  = imm_avl ? vi.IVLI.rd : vi.VLI.rd;
		const int rs1 = imm_avl ? 0 : int(vi.VLI.rs1);

		// Decode with the interpreter's own code, so the two can never
		// disagree about what is legal or how large VLMAX is.
		VectorRegisters<W> newtype;
		const bool legal = newtype.set_vtype(vtypei);
		const uint64_t vlmax = newtype.vlmax();

		this->reset_vector_config();
		if (!legal) {
			// Unsupported vtype: vl = 0 and vtype reads back as vill alone.
			// vsew/lmul keep their old values, as set_vtype leaves them.
			add_code("cpu->rvv.vta = " + std::to_string(newtype.vta()) + ";",
				"cpu->rvv.vma = " + std::to_string(newtype.vma()) + ";",
				"cpu->rvv.vill = 1;", "cpu->rvv.vl = 0;");
			if (rd != 0)
				add_code(to_reg(rd) + " = 0;");
			this->m_vtype = { true, true, 0, 0 };
			this->m_vl_known = true;
			this->m_vl = 0;
			return;
		}
		// A strip-mined loop sets the same vtype every iteration, and only
		// vl changes. The raw encoding decides every other field, so when it
		// is already in place, and not disowned by vill, the configuration
		// is a no-op worth branching over.
		add_code("if (UNLIKELY(cpu->rvv.vtype != " + std::to_string(vtypei)
			+ " || cpu->rvv.vill)) {",
			"  cpu->rvv.vill = 0;",
			"  cpu->rvv.vta = " + std::to_string(newtype.vta()) + ";",
			"  cpu->rvv.vma = " + std::to_string(newtype.vma()) + ";",
			"  cpu->rvv.vsew = " + std::to_string(newtype.encoded_sew()) + ";",
			"  cpu->rvv.lmul = " + std::to_string(newtype.lmul_shift()) + ";",
			"  cpu->rvv.vtype = " + std::to_string(vtypei) + ";",
			"}");

		// AVL: the immediate, x[rs1], VLMAX when rs1 is x0 but rd is not,
		// and otherwise the current vl clamped to the new VLMAX.
		const std::string vl = "cpu->rvv.vl";
		const std::string newvl = "vnewvl" + PCRELS(0);
		if (imm_avl || rs1 == 0) {
			const uint64_t avl = imm_avl ? vi.IVLI.uimm : (rd != 0 ? vlmax : 0);
			if (imm_avl || rd != 0) {
				add_code(vl + " = " + std::to_string(std::min(avl, vlmax)) + ";");
				this->m_vl_known = true;
				this->m_vl = std::min(avl, vlmax);
			} else {
				add_code("if (" + vl + " > " + std::to_string(vlmax) + ") "
					+ vl + " = " + std::to_string(vlmax) + ";");
			}
		} else {
			// Downstream reads vl from this local, not from the field.
			const std::string avl = "vavl" + PCRELS(0);
			this->load_register(rs1);
			add_code("addr_t " + avl + "; " + avl + " = " + from_untracked_reg(rs1) + ";",
				"addr_t " + newvl + "; " + newvl + " = (" + avl + " < "
				+ std::to_string(vlmax) + ") ? " + avl + " : "
				+ std::to_string(vlmax) + ";",
				vl + " = " + newvl + ";");
			this->m_vl_local = newvl;
		}
		this->m_vtype = { true, false, newtype.encoded_sew(), newtype.lmul_shift() };
		if (rd != 0) {
			// Writes the cached register directly: no realize, no reload.
			this->reset_tracked_register(rd);
			if (this->m_vl_known)
				this->track_register_value(rd, this->m_vl);
			add_code(to_reg(rd) + " = " + (m_vl_local.empty() ? vl : m_vl_local) + ";");
		}
	}
	// Emit one vector instruction: inlined when the encoding allows it,
	// otherwise straight to the handler.
	void emit_vector_instruction()
	{
		if (this->vector_is_inlinable_setvl(instr)) {
			this->emit_vector_setvl();
			return;
		}
		// A load or store carries its own EEW and brings its own guard, so
		// it does not go through the SEW machinery the arithmetic needs.
		const auto meminfo = this->vector_memory_form(instr);
		if (meminfo.form != VectorMemForm::None) {
			this->emit_vector_memory(meminfo);
			return;
		}
		if (vector_is_move_xs(instr)) {
			this->emit_vector_move_xs();
			return;
		}
		// Element-wise integer arithmetic, which is most of what a
		// vectorised loop is made of once its loads and stores inline.
		if (const auto intinfo = vector_int_form(instr); intinfo.op != VectorIntOp::None) {
			this->emit_vector_int_arith(intinfo);
			return;
		}
		const auto sews = this->vector_inlinable_sews(instr);
		if (sews.empty()) {
			this->emit_vector_slowpath();
			return;
		}
		// Materialize every guard first: they are C locals, and the bodies
		// below open scopes a later declaration could not escape.
		for (const unsigned vsew : sews)
			(void)this->rvv_guard(vsew);

		for (const unsigned vsew : sews) {
			add_code("if (LIKELY(" + rvv_guard(vsew) + ")) {");
			this->emit_vector_float_arith_body(vsew);
			add_code("} else {");
		}
		this->emit_vector_slowpath();
		for (size_t i = 0; i < sews.size(); i++)
			add_code("}");
	}
#endif
	std::string from_imm(int64_t imm) {
		return std::to_string(imm);
	}
	void emit_op(const std::string& op, const std::string& sop,
		uint32_t rd, uint32_t rs1, const std::string& rs2)
	{
		if (rd == 0) {
			/* must be a NOP */
		} else if (rd == rs1) {
			add_code(to_reg(rd) + sop + rs2 + ";");
		} else {
		add_code(
			to_reg(rd) + " = " + from_reg(rs1) + op + rs2 + ";");
		}
	}

	void emit_branch(const BranchInfo& binfo, const std::string& op);

	void emit_system_call(std::string syscall_reg, bool clobber_all);

	// Returns true if the function call has exited/returned from the block
	bool emit_function_call(address_t target, address_t dest_pc);

	bool gpr_exists_at(int reg) const noexcept { return this->gpr_exists.at(reg); }
	auto& get_gpr_exists() const noexcept { return this->gpr_exists; }

	bool uses_flat_memory_arena() noexcept {
		return riscv::flat_readwrite_arena && tinfo.arena_ptr != 0;
	}
	bool uses_Nbit_encompassing_arena() noexcept {
		if (riscv::encompassing_Nbit_arena != 0 && tinfo.arena_ptr != 0)
			return true;
		if (tinfo.use_automatic_nbit_address_space && tinfo.arena_ptr != 0)
			return true;
		return false;
	}
	constexpr address_t get_Nbit_encompassing_arena_mask() noexcept {
		if constexpr (riscv::encompassing_Nbit_arena != 0)
			return riscv::encompassing_arena_mask;
		else if (tinfo.use_automatic_nbit_address_space)
			return this->m_encompassing_arena_mask;
		else
			return 0;
	}

	std::string arena_at(const std::string& address) {
		// libtcc direct arena pointer access
		// This is a performance optimization for libtcc, which allows direct access to the memory arena
		// however, with it execute segments can no longer be shared between different machines.
		// So, for a simple CLI tool, this is a good optimization. But not for a system of multiple machines.
		if (tinfo.is_libtcc && !tinfo.use_shared_execute_segments) {
			if (uses_Nbit_encompassing_arena()) {
				if (riscv::encompassing_Nbit_arena == 32)
					return "(" + m_arena_hex_address + " + (uint32_t)(" + address + "))";
				else
					return "(" + m_arena_hex_address + " + ((" + address + ") & " + hex_address(address_t(get_Nbit_encompassing_arena_mask())) + "))";
			} else {
				return "(" + m_arena_hex_address + " + " + speculation_safe(address) + ")";
			}
		} else if (uses_Nbit_encompassing_arena()) {
			if constexpr (riscv::encompassing_Nbit_arena == 32)
				return "ARENA_AT(cpu, (uint32_t)(" + address + "))";
			else
				return "ARENA_AT(cpu, (" + address + ") & " + hex_address(address_t(get_Nbit_encompassing_arena_mask())) + ")";
		} else {
			return "ARENA_AT(cpu, " + speculation_safe(address) + ")";
		}
	}

	std::string arena_at_fixed(const std::string& type, address_t address) {
		if (tinfo.is_libtcc && !tinfo.use_shared_execute_segments) {
			if (uses_Nbit_encompassing_arena()) {
				return "*(" + type + "*)" + hex_address(tinfo.arena_ptr + (address & address_t(get_Nbit_encompassing_arena_mask()))) + "";
			} else {
				return "*(" + type + "*)" + hex_address(tinfo.arena_ptr + address) + "";
			}
		} else if (uses_Nbit_encompassing_arena()) {
			return "*(" + type + "*)ARENA_AT(cpu, " + hex_address(address & address_t(get_Nbit_encompassing_arena_mask())) + ")";
		} else {
			return "*(" + type + "*)ARENA_AT(cpu, " + speculation_safe(address) + ")";
		}
	}

	// A bounds check on `base + anchor` proves that address lies inside the arena.
	// The arena is over-allocated by OVERALLOCATE bytes on both ends, so any access
	// through the same base register at an offset within that window of the anchor
	// is also guaranteed to land in mapped memory, and needs no check of its own.
	static bool offset_is_within_overallocation(int64_t anchor, int64_t offset, size_t size) {
		if (UNLIKELY(size > Memory<W>::OVERALLOCATE))
			return false;
		return std::abs(offset - anchor) <= int64_t(Memory<W>::OVERALLOCATE - size);
	}

	// The window stays live until the base register is written, or until control
	// flow can join from somewhere we haven't proven anything about (a label, a
	// call that clobbers registers). Falling through a not-taken branch changes no
	// register, so the window deliberately survives that.
	void invalidate_bounds_checks(int reg) {
		if (reg > 0 && reg < 32) {
			this->m_read_checked[reg].valid = false;
			this->m_write_checked[reg].valid = false;
		}
	}
	void invalidate_all_bounds_checks() {
		for (auto& entry : this->m_read_checked) entry.valid = false;
		for (auto& entry : this->m_write_checked) entry.valid = false;
#ifdef RISCV_EXT_VECTOR
		this->reset_vector_config();
#endif
	}

	bool skip_load_bounds_check(int reg, int64_t offset, size_t size) {
		if (tinfo.unsafe_remove_checks
			|| uses_Nbit_encompassing_arena()) return true; // No bounds check
		if (tinfo.use_virtual_paging_fallback) return false; // Always check

		if (m_read_checked[reg].valid
			&& offset_is_within_overallocation(m_read_checked[reg].anchor, offset, size))
			return true;
		// The arena is divided into unreadable, readable and writable regions,
		// and any region that is writable is also readable, so we inherit the check:
		if (m_write_checked[reg].valid
			&& offset_is_within_overallocation(m_write_checked[reg].anchor, offset, size))
			return true;

		m_read_checked[reg] = { true, offset };
		return false;
	}
	bool skip_store_bounds_check(int reg, int64_t offset, size_t size) {
		if (tinfo.unsafe_remove_checks
			|| uses_Nbit_encompassing_arena()) return true; // No bounds check
		if (tinfo.use_virtual_paging_fallback) return false; // Always check

		// NOTE: A live read check does *not* cover a write: the readable region
		// includes read-only data, which the writable region excludes.
		if (m_write_checked[reg].valid
			&& offset_is_within_overallocation(m_write_checked[reg].anchor, offset, size))
			return true;

		m_write_checked[reg] = { true, offset };
		return false;
	}

	template <typename T>
	std::string memory_load_type(const std::string& address)
	{
		if (std::is_same<T, int8_t>::value || std::is_same<T, uint8_t>::value) {
			return "rd8(cpu, " + address + ");";
		} else if (std::is_same<T, int16_t>::value || std::is_same<T, uint16_t>::value) {
			return "rd16(cpu, " + address + ");";
		} else if (std::is_same<T, int32_t>::value || std::is_same<T, uint32_t>::value) {
			return "rd32(cpu, " + address + ");";
		} else if (std::is_same<T, int64_t>::value || std::is_same<T, uint64_t>::value) {
			return "rd64(cpu, " + address + ");";
		} else {
			throw MachineException(INVALID_PROGRAM, "Unsupported memory load type");
		}
	}
	template <typename T>
	void memory_load(std::string dst, std::string type, int reg, int32_t imm)
	{
		if (uses_flat_memory_arena()) {
			address_t absolute_vaddr = 0;
			if (reg == REG_GP && tinfo.gp != 0x0) {
				absolute_vaddr = tinfo.gp + imm;
			}
			constexpr bool good = riscv::encompassing_Nbit_arena != 0;
			if (absolute_vaddr != 0 && absolute_vaddr >= 0x1000 && (good || absolute_vaddr + sizeof(T) <= tinfo.arena_size)) {
				add_code(
					dst + " = " + arena_at_fixed(type, absolute_vaddr) + ";"
				);
				return;
			}
			if (auto tracked_value = get_tracked_register(reg)) {
				const address_t vaddr = *tracked_value + imm;
				if (vaddr >= 0x1000 && vaddr + sizeof(T) <= tinfo.arena_size) {
					add_code(dst + " = " + arena_at_fixed(type, vaddr) + ";");
					return;
				}
			}
		}

		const std::string address = from_untracked_reg(reg) + " + " + from_imm(imm);
		if (skip_load_bounds_check(reg, imm, sizeof(T)))
		{
			add_code(dst + " = *(" + type + "*)" + arena_at(address) + ";");
		}
		else if (uses_flat_memory_arena()) {
			add_code(
				"if (LIKELY(ARENA_READABLE(" + address + ")))",
					dst + " = *(" + type + "*)" + arena_at(address) + ";",
				"else {");
			if (!tinfo.use_virtual_paging_fallback) {
				add_code("  cpu->pc = " + hex_address(this->pc()) + "LL; goto exception;",
						"}");
				return;
			}
			else if ((W == 8 && (type == "int64_t" || type == "uint64_t"))
				|| (W == 4 && (type == "int32_t" || type == "uint32_t"))) {
				add_code(
					dst + " = " + memory_load_type<T>(address) + ";",
				"}");
			} else {
				add_code(
					dst + " = (" + type + ")" + memory_load_type<T>(address) + ";",
				"}");
			}
		} else {
			add_code(
				dst + " = (" + type + ")" + memory_load_type<T>(address) + ";"
			);
		}
	}
	std::string memory_store_type(const std::string& type, const std::string& address, const std::string& value)
	{
		if (type == "int8_t" || type == "uint8_t") {
			return "wr8(cpu, " + address + ", " + value + ");";
		} else if (type == "int16_t" || type == "uint16_t") {
			return "wr16(cpu, " + address + ", " + value + ");";
		} else if (type == "int32_t" || type == "uint32_t") {
			return "wr32(cpu, " + address + ", " + value + ");";
		} else if (type == "int64_t" || type == "uint64_t") {
			return "wr64(cpu, " + address + ", " + value + ");";
		} else {
			throw MachineException(INVALID_PROGRAM, "Unsupported memory store type");
		}
	}
	// NOTE: TCC truncates a 64-bit absolute store address to a 32-bit displacement,
	// so the pointer must be materialized first. It goes in one function-scope
	// scratch, as TCC never reuses the stack slot of a block-scope local.
	void fixed_store(const std::string& type, address_t address, const std::string& value)
	{
		this->m_used_fixed_store = true;
		add_code("mstore = (char*)&" + arena_at_fixed(type, address) + ";",
			"*(" + type + "*)mstore = " + value + ";");
	}
	// NOTE: `size` is the width of the access, and must be passed explicitly --
	// it used to be derived as sizeof(type), which is sizeof(std::string).
	void memory_store(std::string type, size_t size, int reg, int32_t imm, std::string value)
	{
		if (uses_flat_memory_arena()) {
			address_t absolute_vaddr = 0;
			if (reg == REG_GP && tinfo.gp != 0x0) {
				absolute_vaddr = tinfo.gp + imm;
			}
			constexpr bool good = riscv::encompassing_Nbit_arena != 0;
			if (absolute_vaddr != 0 && absolute_vaddr >= tinfo.arena_roend && (good || absolute_vaddr < tinfo.arena_size)) {
				fixed_store(type, absolute_vaddr, value);
				return;
			}
			if (auto tracked_value = get_tracked_register(reg)) {
				const address_t vaddr = *tracked_value + imm;
				if (vaddr >= tinfo.arena_roend && vaddr <= tinfo.arena_size - 32) {
					fixed_store(type, vaddr, value);
					return;
				}
			}
		}

		const std::string address = from_untracked_reg(reg) + " + " + from_imm(imm);
		if (skip_store_bounds_check(reg, imm, size))
		{
			add_code("*(" + type + "*)" + arena_at(address) + " = " + value + ";");
		}
		else if (uses_flat_memory_arena()) {
			add_code(
				"if (LIKELY(ARENA_WRITABLE(" + address + ")))",
				"  *(" + type + "*)" + arena_at(address) + " = " + value + ";",
				"else {");
			if (!tinfo.use_virtual_paging_fallback) {
				add_code("  cpu->pc = " + hex_address(this->pc()) + "LL; goto exception;",
						"}");
			} else {
				add_code("  " + memory_store_type(type, address, value) + ";",
						"}");
			}
		} else {
			add_code(
				memory_store_type(type, address, value)
			);
		}
	}

	bool no_labels_after_this() const noexcept {
		for (auto addr : labels)
			if (addr > this->pc())
				return false;
		for (auto addr : tinfo.jump_locations)
			if (addr > this->pc())
				return false;
		return true;
	}

	void add_mapping(address_t addr, std::string symbol) { this->mappings.push_back({addr, std::move(symbol)}); }
	auto& get_mappings() { return this->mappings; }

	bool add_reentry_next() {
		// Avoid re-entering at the end of the function
		// WARNING: End-of-function can be empty
		if (this->pc() + this->m_instr_length >= end_pc())
			return false;
		this->mapping_labels.insert(index() + 1);
		//code.append(FUNCLABEL(this->pc() + 4) + ":;\n");
		return true;
	}

	uint64_t reset_and_get_icounter() {
		auto result = this->m_instr_counter;
		this->m_instr_counter = 0;
		return result;
	}
	void increment_counter_so_far() {
		auto icount = this->reset_and_get_icounter();
		if (icount > 0 && !tinfo.ignore_instruction_limit)
			code.append("ic += " + std::to_string(icount) + ";\n");
	}
	void penalty(uint64_t cycles) {
		this->m_instr_counter += cycles;
	}

	bool block_exists(address_t pc) const noexcept {
		for (auto& blk : *tinfo.blocks) {
			if (blk.basepc == pc) return true;
		}
		return false;
	}
	uint64_t find_block_base(address_t pc) const noexcept {
		for (auto& blk : *tinfo.blocks) {
			if (pc >= blk.basepc && pc < blk.endpc) return blk.basepc;
		}
		return 0;
	}

	void add_forward(const std::string& target_func) {
		this->m_forward_declared.push_back(target_func);
	}
	const auto& get_forward_declared() const noexcept { return this->m_forward_declared; }

	size_t index() const noexcept { return this->m_idx; }
	address_t pc() const noexcept { return this->m_pc; }
	address_t begin_pc() const noexcept { return tinfo.basepc; }
	address_t end_pc() const noexcept { return tinfo.endpc; }

	bool within_segment(address_t addr) const noexcept {
		return addr >= this->tinfo.segment_basepc && addr < this->tinfo.segment_endpc;
	}
	bool used_store_syscalls() const noexcept { return this->m_used_store_syscalls; }
	bool used_fixed_store() const noexcept { return this->m_used_fixed_store; }

	const std::string get_func() const noexcept { return this->func; }
	void emit();
	rv32i_instruction emit_rvc();

private:
	static std::string speculation_safe(const std::string& address) {
		return "SPECSAFE(" + address + ")";
	}
	static std::string speculation_safe(const address_t address) {
		return "SPECSAFE(" + hex_address(address) + ")";
	}
	std::optional<address_t> get_tracked_register(int idx) const {
		if (idx < 0 || idx >= 32) {
			throw MachineException(INVALID_PROGRAM, "Invalid register index for tracking", idx);
		}
		if (this->m_is_tracked_register[idx]) {
			return this->m_tracked_registers[idx];
		}
		return std::nullopt;
	}
	void track_register_value(int idx, address_t value) {
		if (idx < 0 || idx >= 32) {
			throw MachineException(INVALID_PROGRAM, "Invalid register index for tracking", idx);
		}
		else if (idx > 0) {
			this->m_tracked_registers[idx] = value;
			this->m_is_tracked_register[idx] = true;
		}
	}
	void reset_tracked_register(int idx) {
		if (idx < 0 || idx >= 32) {
			throw MachineException(INVALID_PROGRAM, "Invalid register index for tracking", idx);
		}
		this->m_is_tracked_register[idx] = false;
		this->invalidate_bounds_checks(idx);
	}
	// Forgets tracked constants only. The caller must decide separately whether the
	// bounds-check windows also die: they survive a not-taken branch, but not a
	// label, a call, or anything else that can change registers behind our back.
	void reset_all_tracked_constants() {
		this->m_is_tracked_register.fill(false);
	}
	void reset_all_tracked_registers() {
		this->reset_all_tracked_constants();
		this->invalidate_all_bounds_checks();
	}

	std::string code;
	size_t m_idx = 0;
	address_t m_pc = 0x0;
	address_t m_last_pc = 0x0;
	rv32i_instruction instr;
	unsigned m_instr_length = 0;
	uint64_t m_instr_counter = 0;
	uint32_t m_zero_insn_counter = 0;
#ifdef RISCV_EXT_VECTOR
	// vtype as proven by an inlined vsetvli earlier in this block. Dropped
	// where control flow joins, or where a handler may reconfigure it.
	struct KnownVtype {
		bool     known = false;
		bool     vill  = false;
		unsigned vsew  = 0;  // log2(SEW / 8)
		int      lmul  = 0;  // log2(LMUL)
	};
	KnownVtype m_vtype;
	// vl when the vsetvli that produced it had a compile-time AVL
	bool     m_vl_known = false;
	uint64_t m_vl = 0;
	// C local holding the live vl, when an inlined vsetvli computed one
	std::string m_vl_local;
	// Live C local holding the vl/vtype fast-path test per SEW, see rvv_guard()
	std::array<std::string, 4> m_rvv_guard;
	// ... and the same for the integer bodies, see rvv_int_guard()
	std::array<std::string, 4> m_rvv_int_guard;
#endif
	address_t m_encompassing_arena_mask = 0;
	bool m_used_store_syscalls = false;
	bool m_used_fixed_store = false;

	// Per-register live bounds-check window (see offset_is_within_overallocation)
	struct BoundsCheck {
		bool    valid  = false;
		int64_t anchor = 0;
	};
	std::array<BoundsCheck, 32> m_read_checked {};
	std::array<BoundsCheck, 32> m_write_checked {};

	std::array<bool, 32> gpr_exists {};
	std::array<bool, 32> m_is_tracked_register {};
	std::array<address_t, 32> m_tracked_registers {};

	std::string func;
	const TransInfo<W>& tinfo;
	std::string m_arena_hex_address;

	std::vector<TransMapping<W>> mappings;
	std::unordered_set<unsigned> labels;
	std::unordered_set<unsigned> mapping_labels;
	std::unordered_set<address_t> pagedata;

	std::vector<std::string> m_forward_declared;
};

template <int W>
inline void Emitter<W>::emit_branch(const BranchInfo& binfo, const std::string& op)
{
	using address_t = address_type<W>;
	if (binfo.sign == false)
		code += "if (" + from_reg(instr.Btype.rs1) + op + from_reg(instr.Btype.rs2) + ")";
	else
		code += "if ((saddr_t)" + from_reg(instr.Btype.rs1) + op + " (saddr_t)" + from_reg(instr.Btype.rs2) + ")";

	if (UNLIKELY(PCRELA(instr.Btype.signed_imm()) & ALIGN_MASK))
	{
		// TODO: Make exception a helper function, as return values are implementation detail
		code += "\n  { api.exception(cpu, " + PCRELS(0) + ", MISALIGNED_INSTRUCTION); RETURN_VALUES(0, 0); }\n";
		return;
	}

	if (binfo.jump_pc != 0) {
		if (binfo.jump_pc > this->pc() || binfo.ignore_instruction_limit) {
			// unconditional forward jump + bracket
			code += " goto " + FUNCLABEL(binfo.jump_pc) + ";\n";
			return;
		}
		// backward jump
		code += " {\nif (" + LOOP_EXPRESSION + ") goto " + FUNCLABEL(binfo.jump_pc) + ";\n";
	} else if (binfo.call_pc != 0 && binfo.call_pc > this->pc()) {
		code += " {\n";
		// potentially call a function
		auto target_funcaddr = this->find_block_base(binfo.call_pc);
		// Allow directly calling a function, as long as it's a forward branch
		if (target_funcaddr != 0) {
			emit_function_call(target_funcaddr, binfo.call_pc);
			code += "}\n"; // Bracket (NOTE: not ending the function, just the branch)
			return;
		}
	} else {
		code += " {\n";
	}
	// else, exit binary translation
	exit_function(PCRELS(instr.Btype.signed_imm()), true); // Bracket (NOTE: not actually ending the function)
}

template <int W>
inline bool Emitter<W>::emit_function_call(address_t target_funcaddr, address_t dest_pc)
{
	// The callee may write any register
	this->invalidate_all_bounds_checks();
	// Store the registers
	this->store_loaded_registers();

	auto target_func = funclabel<W>("f", target_funcaddr);
	add_forward(target_func);
	if (!tinfo.ignore_instruction_limit) {
		// Call the function and get the return values
		add_code("retvals = " + target_func + "(cpu, ic, max_ic, " + STRADDR(dest_pc) + ");");
		// Update the local counter registers
		add_code("ic = retvals.ic; max_ic = retvals.max_ic;");
	} else {
		add_code("retvals = " + target_func + "(cpu, 0, max_ic, " + STRADDR(dest_pc) + ");");
		add_code("max_ic = retvals.max_ic;");
	}

	// Restore the registers
	this->reload_all_registers();

	if (tinfo.trace_instructions) {
		code += "api.trace(cpu, \"" + this->func + "\", cpu->pc, max_ic);\n";
	}

	// Hope and pray that the next PC is local to this block
	if (!tinfo.ignore_instruction_limit) {
		add_code("if (" + LOOP_EXPRESSION + ") { pc = cpu->pc; goto " + this->func + "_jumptbl; }");
		add_code("RETURN_VALUES(ic, max_ic);");
	} else {
		add_code("if (max_ic) { pc = cpu->pc; goto " + this->func + "_jumptbl; }");
		add_code("RETURN_VALUES(0, 0);");
	}
	return true;
}

template <int W>
inline void Emitter<W>::emit_system_call(std::string syscall_reg, bool clobber_all)
{
	if (auto tracked_value = get_tracked_register(17); tracked_value) {
		// Don't clobber when the value is known and it's not in the list
		// of known system calls that clobber all registers
		if (tinfo.use_syscall_clobbering_optimization && this->uses_register_caching() && !clobber_all) {
			clobber_all = Machine<W>::is_clobbering_syscall(*tracked_value);
		} else {
			clobber_all = true;
		}

		if (syscall_reg != std::to_string(SYSCALL_EBREAK)) {
			if constexpr (W != 16) { // No 128-bit to_string in C++
				syscall_reg = std::to_string(*tracked_value);
			}
		}
	} else {
		clobber_all = true;
	}
	if (clobber_all) {
		this->store_loaded_registers();
	} else {
		this->store_syscall_registers();
	}
	if (tinfo.is_libtcc)
	{
		if (!tinfo.ignore_instruction_limit) {
			code += "max_ic = api.system_call(cpu, " + PCRELS(0) + ", ic, max_ic, " + syscall_reg + ");\n";
			code += "ic = INS_COUNTER(cpu);\n";
		} else {
			code += "max_ic = api.system_call(cpu, " + PCRELS(0) + ", 0, max_ic, " + syscall_reg + ");\n";
		}
		code += "if (!max_ic) {\n";
		if (this->uses_register_caching() && !clobber_all)
		{
			// Non-clobbering syscall, but we are about to leave, so
			// restore all the remaining registers
			if (!tinfo.ignore_instruction_limit) {
				code += "max_ic = MAX_COUNTER(cpu);\n"
						"if (ic >= max_ic) {\n"
						"  STORE_NON_SYS_REGS_" + this->func + "();\n"
						"}\n"
						"  RETURN_VALUES(ic, max_ic);\n"
						"}\n";
			} else {
				code += "max_ic = MAX_COUNTER(cpu);\n"
						"if (max_ic == 0) {\n"
						"  STORE_NON_SYS_REGS_" + this->func + "();\n"
						"}\n"
						"  RETURN_VALUES(0, max_ic);\n"
						"}\n";
			}
		}
		else if (!tinfo.ignore_instruction_limit) {
			code += "  RETURN_VALUES(ic, MAX_COUNTER(cpu));\n"
					"}\n";
		} else {
			code += "  RETURN_VALUES(0, MAX_COUNTER(cpu));\n"
					"}\n";
		}
	}
	else
	{
		code += "cpu->pc = " + PCRELS(0) + ";\n";
		if (!tinfo.ignore_instruction_limit) {
			code += "if (UNLIKELY(do_syscall(cpu, ic, max_ic, " + syscall_reg + "))) {\n";
			if (this->uses_register_caching() && !clobber_all)
			{
				// If we didn't clobber all registers, and the machine timed out,
				// we need to store back the registers so that the timed out machine
				// can resume from where it left off, if it is re-entered.
				code += "if (ic >= MAX_COUNTER(cpu)) {\n";
				code += "  STORE_NON_SYS_REGS_" + this->func + "();\n";
				code += "}\n";
			}
			code += "  cpu->pc += 4; RETURN_VALUES(ic, MAX_COUNTER(cpu));}\n"; // Correct for +4 expectation outside of bintr
			code += "max_ic = MAX_COUNTER(cpu);\n"; // Restore max counter
		} else {
			code += "if (UNLIKELY(do_syscall(cpu, 0, max_ic, " + syscall_reg + "))) {\n";
			code += "  cpu->pc += 4; RETURN_VALUES(0, MAX_COUNTER(cpu));}\n";
		}
	}
	if (clobber_all) {
		this->reset_all_tracked_registers();
	} else {
		this->reset_tracked_register(10);
		this->reset_tracked_register(11);
	}
#ifdef RISCV_EXT_VECTOR
	this->reset_vector_config();
#endif
	this->reload_syscall_registers();
}

#ifdef RISCV_EXT_C
#include "tr_emit_rvc.cpp"
#endif

template <int W>
void Emitter<W>::emit()
{
	this->add_mapping(this->pc(), this->func);
	code.append(FUNCLABEL(this->pc()) + ":;\n");
	auto next_pc = tinfo.basepc;
	address_t current_callable_pc = 0;
	this->m_pc = tinfo.basepc;
	this->m_last_pc = tinfo.basepc;

	for (int i = 0; i < int(tinfo.instr.size()); i++) {
		this->m_idx = i;
		this->instr = tinfo.instr[i];
		this->m_last_pc = this->m_pc;
		this->m_pc = next_pc;
		if constexpr (compressed_enabled)
			this->m_instr_length = this->instr.length();
		else
			this->m_instr_length = 4;
		next_pc = this->m_pc + this->m_instr_length;

		if (this->instr.is_illegal()) {
			this->m_zero_insn_counter ++;
		} else {
			if (this->m_zero_insn_counter >= 4) {
				// After a ream of zero instructions, we predict a jump target
				mapping_labels.insert(i);
			}
			this->m_zero_insn_counter = 0;
		}

		// If the address is a return address or a global JAL target
		if (i > 0 && (mapping_labels.count(i) || tinfo.global_jump_locations.count(this->pc()))) {
			this->increment_counter_so_far();
			// Re-entry through the current function
			code.append(FUNCLABEL(this->pc()) + ":;\n");
			this->mappings.push_back({
				this->pc(), this->func
			});
			// Since someone can jump here, we need to forget all tracked register values
			this->reset_all_tracked_registers();
		}
		// known jump locations
		else if (i > 0 && tinfo.jump_locations.count(this->pc())) {
			this->increment_counter_so_far();
			code.append(FUNCLABEL(this->pc()) + ":;\n");
			// Since someone can jump here, we need to forget all tracked register values
			this->reset_all_tracked_registers();
		}

		// With garbage instructions, it's possible that someone is trying to jump to
		// the middle of an instruction. This technically allowed, so we need to check
		// there's a jump label in the middle of this instruction.
		if (UNLIKELY(compressed_enabled && this->m_instr_length == 4 && tinfo.jump_locations.count(this->pc() + 2))) {
			// This occurence should be very rare, so we permit outselves to jump over it, so that
			// we can trigger an exception for anyone trying to jump to the middle of an instruction.
			// It is technically possible to create an endless loop without this, as we are not
			// counting instructions correctly for this case.
			code.append("goto " + FUNCLABEL(this->pc() + 2) + "_skip;\n");
			code.append(FUNCLABEL(this->pc() + 2) + ":;\n");
			code.append("api.exception(cpu, " + STRADDR(this->pc() + 2) + ", MISALIGNED_INSTRUCTION); RETURN_VALUES(0, 0);\n");
			code.append(FUNCLABEL(this->pc() + 2) + "_skip:;\n");
			this->reset_all_tracked_registers();
		}

		auto it = tinfo.single_return_locations.find(this->pc());
		if (it != tinfo.single_return_locations.end()) {
			// We don't know what function we are in, but we do know what functions get called
			// Track the current callable PC, so that we can use that for JALR return addresses
			// If the address is zero, it means many places call this function, so we can't predict
			// a single return address.
			if (it->second != 0)
				current_callable_pc = this->pc();
			else
				current_callable_pc = 0;
		}

		this->m_instr_counter += 1;

		if (tinfo.trace_instructions) {
			char buffer[128];
			const int len = snprintf(buffer, sizeof(buffer),
				"api.trace(cpu, \"%s\", 0x%" PRIx64 ", 0x%X);\n",
				this->func.c_str(), uint64_t(this->pc()), instr.whole);
			code.append(buffer, len);
		}

		if (tinfo.ebreak_locations->count(this->pc())) {
			this->emit_system_call(std::to_string(SYSCALL_EBREAK), true);
		}

		// instruction generation
#ifdef RISCV_EXT_C
		if (instr.is_compressed()) {
			// Compressed 16-bit instructions
			auto original = instr.whole;
			instr = this->emit_rvc();

			if (instr.is_compressed())
			{
				const uint16_t compressed_instr = instr.half[0];
				// Unexpanded instruction (except all-zeroes, which is illegal)
				if (tinfo.trace_instructions && compressed_instr != 0x0)
					printf("Unexpanded instruction: 0x%04hx at PC 0x%lX (original 0x%x)\n", compressed_instr, long(this->pc()), original);
				// When illegal opcode is encountered, reveal PC
				if (m_zero_insn_counter <= 1 || compressed_instr != 0x0) {
					code += "api.exception(cpu, " + STRADDR(this->pc()) + ", ILLEGAL_OPCODE);\n";
					if (tinfo.is_libtcc) {
						code += "RETURN_VALUES(0, 0);\n";
					}
				}
				this->reset_all_tracked_registers();
				continue;
			}
		}
#endif

		switch (instr.opcode()) {
		case RV32I_LOAD:
			load_register(instr.Itype.rs1);
			if (instr.Itype.rd != 0) {
			switch (instr.Itype.funct3) {
			case 0x0: // I8
				this->memory_load<int8_t>(to_reg(instr.Itype.rd), "int8_t", instr.Itype.rs1, instr.Itype.signed_imm());
				break;
			case 0x1: // I16
				this->memory_load<int16_t>(to_reg(instr.Itype.rd), "int16_t", instr.Itype.rs1, instr.Itype.signed_imm());
				break;
			case 0x2: // I32
				this->memory_load<int32_t>(to_reg(instr.Itype.rd), "int32_t", instr.Itype.rs1, instr.Itype.signed_imm());
				break;
			case 0x3: // I64
				this->memory_load<int64_t>(to_reg(instr.Itype.rd), "int64_t", instr.Itype.rs1, instr.Itype.signed_imm());
				break;
			case 0x4: // U8
				this->memory_load<uint8_t>(to_reg(instr.Itype.rd), "uint8_t", instr.Itype.rs1, instr.Itype.signed_imm());
				break;
			case 0x5: // U16
				this->memory_load<uint16_t>(to_reg(instr.Itype.rd), "uint16_t", instr.Itype.rs1, instr.Itype.signed_imm());
				break;
			case 0x6: // U32
				this->memory_load<uint32_t>(to_reg(instr.Itype.rd), "uint32_t", instr.Itype.rs1, instr.Itype.signed_imm());
				break;
			default:
				UNKNOWN_INSTRUCTION();
			}
			this->reset_tracked_register(instr.Itype.rd);
			} else {
				// We don't care about where we are in the page when rd=0
				const auto temp = "tmp" + PCRELS(0);
				add_code("uint8_t " + temp + ";");
				this->memory_load<uint8_t>(temp, "volatile uint8_t", instr.Itype.rs1, instr.Itype.signed_imm());
				add_code("(void)" + temp + ";");
			} break;
		case RV32I_STORE:
			load_register(instr.Stype.rs1);
			switch (instr.Stype.funct3) {
			case 0x0: // I8
				this->memory_store("int8_t", sizeof(int8_t), instr.Stype.rs1, instr.Stype.signed_imm(), from_reg(instr.Stype.rs2));
				break;
			case 0x1: // I16
				this->memory_store("int16_t", sizeof(int16_t), instr.Stype.rs1, instr.Stype.signed_imm(), from_reg(instr.Stype.rs2));
				break;
			case 0x2: // I32
				this->memory_store("int32_t", sizeof(int32_t), instr.Stype.rs1, instr.Stype.signed_imm(), from_reg(instr.Stype.rs2));
				break;
			case 0x3: // I64
				this->memory_store("int64_t", sizeof(int64_t), instr.Stype.rs1, instr.Stype.signed_imm(), from_reg(instr.Stype.rs2));
				break;
			default:
				UNKNOWN_INSTRUCTION();
			}
			break;
		case RV32I_BRANCH: {
			this->increment_counter_so_far();
			load_register(instr.Btype.rs1);
			load_register(instr.Btype.rs2);
			const auto offset = instr.Btype.signed_imm();
			uint64_t dest_pc = this->pc() + offset;
			uint64_t jump_pc = 0;
			uint64_t call_pc = 0;
			// goto branch: restarts function
			if (dest_pc == this->begin_pc()) {
				// restart function
				jump_pc = dest_pc;
			}
			// forward label: branch inside code block
			else if (offset > 0 && dest_pc < this->end_pc()) {
				// forward label: future address
				labels.insert(dest_pc);
				jump_pc = dest_pc;
			} else if (tinfo.jump_locations.count(dest_pc)) {
				// existing jump location
				if (dest_pc >= this->begin_pc() && dest_pc < this->end_pc()) {
					jump_pc = dest_pc;
				}
			} else if (tinfo.global_jump_locations.count(dest_pc) && this->within_segment(dest_pc)) {
				// global jump location
				call_pc = dest_pc;
			}
			switch (instr.Btype.funct3) {
			case 0x0: // EQ
				emit_branch({ false, tinfo.ignore_instruction_limit, jump_pc, call_pc }, " == ");
				break;
			case 0x1: // NE
				emit_branch({ false, tinfo.ignore_instruction_limit, jump_pc, call_pc }, " != ");
				break;
			case 0x2:
			case 0x3:
				UNKNOWN_INSTRUCTION();
				break;
			case 0x4: // LT
				emit_branch({ true, tinfo.ignore_instruction_limit, jump_pc, call_pc }, " < ");
				break;
			case 0x5: // GE
				emit_branch({ true, tinfo.ignore_instruction_limit, jump_pc, call_pc }, " >= ");
				break;
			case 0x6: // LTU
				emit_branch({ false, tinfo.ignore_instruction_limit, jump_pc, call_pc }, " < ");
				break;
			case 0x7: // GEU
				emit_branch({ false, tinfo.ignore_instruction_limit, jump_pc, call_pc }, " >= ");
				break;
			}
			// Only the code after this branch falls through here, and a not-taken
			// branch writes no register -- so the bounds-check windows survive it.
			// (The taken side always leaves via goto/return, and every place it can
			// land emits a label, where the windows are invalidated anyway.)
			this->reset_all_tracked_constants(); // For now
			} break;
		case RV32I_JALR: {
			// jump to register + immediate
			this->reset_all_tracked_registers(); // For now
			this->increment_counter_so_far();
			if (instr.Itype.rd != 0 && instr.Itype.rd == instr.Itype.rs1) {
				// NOTE: We need to remember RS1 because it is clobbered by RD
				const auto src = from_reg(instr.Itype.rs1);
				const auto dst = to_reg(instr.Itype.rd);
				add_code(
					"JUMP_TO(" + src + " + " + from_imm(instr.Itype.signed_imm()) + ");",
					dst + " = " + PCRELS(m_instr_length) + ";"
				);
			} else if (instr.Itype.rd != 0) {
				add_code(
					to_reg(instr.Itype.rd) + " = " + PCRELS(m_instr_length) + ";",
					"JUMP_TO(" + from_reg(instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");"
				);
			} else {
				// If this is JALR ra, check if the return address is a single return location
				if (instr.Itype.rs1 != 0 && instr.Itype.signed_imm() == 0 && current_callable_pc != 0) {
					// Return locations are stored from the callee's perspective
					auto it = tinfo.single_return_locations.find(current_callable_pc);
					if (it == tinfo.single_return_locations.end()) {
						throw std::runtime_error("JALR ra with current callable PC, without a return location");
					}
					// TODO: Check if the return location is in the current block
					// If it is, we can jump directly to it
					// Otherwise, we should immediately exit the function
					//printf("Single return location: 0x%lX (pc=0x%lX) -> 0x%lX\n",
					//	long(current_callable_pc), long(this->pc()), long(it->second));
					if (it->second >= this->begin_pc() && it->second < this->end_pc()) {
						// Jump directly to the return location
						add_code("if (" + from_reg(instr.Itype.rs1) + " == " + STRADDR(current_callable_pc) + ") goto " + FUNCLABEL(it->second) + ";");
					}
					// Otherwise, we need to use unknown register values to jump
				}
				add_code(
					"JUMP_TO(" + from_reg(instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");"
				);
			}
			// Untrack current callable PC
			current_callable_pc = 0;
			if (!tinfo.ignore_instruction_limit)
				code += "if (pc >= " + STRADDR(this->begin_pc()) + " && pc < " + STRADDR(this->end_pc()) + " && " + LOOP_EXPRESSION + ") goto " + this->func + "_jumptbl;\n";
			else
				code += "if (pc >= " + STRADDR(this->begin_pc()) + " && pc < " + STRADDR(this->end_pc()) + ") goto " + this->func + "_jumptbl;\n";
			exit_function("pc", false);
			this->add_reentry_next();
			} break;
		case RV32I_JAL: {
			this->reset_all_tracked_registers(); // For now
			this->increment_counter_so_far();
			if (instr.Jtype.rd != 0) {
				add_code(to_reg(instr.Jtype.rd) + " = " + PCRELS(m_instr_length) + ";\n");
			}
			// XXX: mask off unaligned jumps - is this OK?
			const auto dest_pc = (this->pc() + instr.Jtype.jump_offset()) & ~address_t(ALIGN_MASK);
			bool add_reentry = instr.Jtype.rd != 0;
			bool already_exited = false;
			// forward label: jump inside code block
			if (dest_pc >= this->begin_pc() && dest_pc < this->end_pc()) {
				// forward labels require creating future labels
				if (dest_pc > this->pc()) {
					labels.insert(dest_pc);
					add_code("goto " + FUNCLABEL(dest_pc) + ";");
					already_exited = true; // Unconditional jump
				} else if (tinfo.ignore_instruction_limit) {
					// jump backwards: without counters
					add_code("goto " + FUNCLABEL(dest_pc) + ";");
					// Random jumps around often have useful code immediately after,
					// so make sure it's accessible (add a re-entry point)
					// TODO: Check if the next instruction is a public symbol address
					if (instr.Jtype.rd == 0)
						add_reentry = true;
					already_exited = true; // Unconditional jump
				} else {
					// jump backwards: use counters
					add_code("if (" + LOOP_EXPRESSION + ") goto " + FUNCLABEL(dest_pc) + ";");
					// Random jumps around often have useful code immediately after,
					// so make sure it's accessible (add a re-entry point)
					// TODO: Check if the next instruction is a public symbol address
					if (instr.Jtype.rd == 0)
						add_reentry = true;
				}
				// .. if we run out of instructions, we must jump manually and exit:
			}
			else if (this->tinfo.global_jump_locations.count(dest_pc) && this->within_segment(dest_pc)) {
				// Get the function name of the target block
				auto target_funcaddr = this->find_block_base(dest_pc);
				// Allow directly calling a function, as long as it's a forward jump
				/// XXX: This forward call is buggy, and crashes on Windows with LIBTCC
				/// Don't enable until it is fixed (or well understood)
				if (false && target_funcaddr != 0 && dest_pc > this->pc()) {
					//printf("Jump location OK (forward): 0x%lX for block 0x%lX -> 0x%lX\n", long(dest_pc),
					//	long(this->begin_pc()), long(this->end_pc()));
					already_exited = this->emit_function_call(target_funcaddr, dest_pc);

					if (!already_exited)
						exit_function("cpu->pc", false);
					already_exited = true;
				} else {
					//printf("Jump location inconvenient (backward): 0x%lX at func 0x%lX for block 0x%lX -> 0x%lX\n",
					//	long(dest_pc), long(target_funcaddr), long(this->begin_pc()), long(this->end_pc()));
				}
			}

			// Because of forward jumps we can't end the function here
			if (!already_exited)
				exit_function(STRADDR(dest_pc), false);
			if (add_reentry)
				this->add_reentry_next();
			} break;

		case RV32I_OP_IMM: {
			// NOP: Instruction without side-effect
			if (UNLIKELY(instr.Itype.rd == 0)) break;

			const auto dst = to_reg(instr.Itype.rd);
			std::string src = from_reg(instr.Itype.rs1);

			switch (instr.Itype.funct3) {
			case 0x0: // ADDI
				if (instr.Itype.signed_imm() == 0) {
					add_code(dst + " = " + src + ";");
				} else if (instr.Itype.rs1 == 0) {
					add_code(dst + " = " + from_imm(instr.Itype.signed_imm()) + ";");
				} else {
					emit_op(" + ", " += ", instr.Itype.rd, instr.Itype.rs1, from_imm(instr.Itype.signed_imm()));
				}
				break;
			case 0x1: // SLLI
				// SLLI: Logical left-shift 5/6-bit immediate
				switch (instr.Itype.imm) {
				case 0b011000000100: // SEXT.B
					add_code(
						dst + " = (int8_t)" + src + ";");
					break;
				case 0b011000000101: // SEXT.H
					add_code(
						dst + " = (int16_t)" + src + ";");
					break;
				case 0b011000000000: // CLZ
					if constexpr (W == 4)
						add_code(
							dst + " = " + src + " ? do_clz(" + src + ") : XLEN;");
					else
						add_code(
							dst + " = " + src + " ? do_clzl(" + src + ") : XLEN;");
					break;
				case 0b011000000001: // CTZ
					if constexpr (W == 4)
						add_code(
							dst + " = " + src + " ? do_ctz(" + src + ") : XLEN;");
					else
						add_code(
							dst + " = " + src + " ? do_ctzl(" + src + ") : XLEN;");
					break;
				case 0b011000000010: // CPOP
					if constexpr (W == 4)
						add_code(
							dst + " = do_cpop(" + src + ");");
					else
						add_code(
							dst + " = do_cpopl(" + src + ");");
					break;
				default:
					if (instr.Itype.high_bits() == 0 && (W != 4 || (instr.Itype.imm & 0x20) == 0)) {
						// SLLI: Logical left-shift immediate
						emit_op(" << ", " <<= ", instr.Itype.rd, instr.Itype.rs1,
							std::to_string(instr.Itype.shift64_imm() & (XLEN-1)));
					} else if (instr.Itype.high_bits() == 0x280 && (W != 4 || (instr.Itype.imm & 0x20) == 0)) {
						// BSETI: Bit-set immediate
						add_code(dst + " = " + src + " | ((addr_t)1 << (" + std::to_string(instr.Itype.imm & (XLEN-1)) + "));");
					}
					else if (instr.Itype.high_bits() == 0x480 && (W != 4 || (instr.Itype.imm & 0x20) == 0)) {
						// BCLRI: Bit-clear immediate
						add_code(dst + " = " + src + " & ~((addr_t)1 << (" + std::to_string(instr.Itype.imm & (XLEN-1)) + "));");
					}
					else if (instr.Itype.high_bits() == 0x680 && (W != 4 || (instr.Itype.imm & 0x20) == 0)) {
						// BINVI: Bit-invert immediate
						add_code(dst + " = " + src + " ^ ((addr_t)1 << (" + std::to_string(instr.Itype.imm & (XLEN-1)) + "));");
					}
					else if (W == 4 && instr.Itype.imm == 0b000010001111) {
						add_code(dst + " = do_zip32(" + src + ");");
					} else {
						UNKNOWN_INSTRUCTION();
					}
				}
				break;
			case 0x2: // SLTI
				// SLTI: Set less than immediate
				add_code(
					dst + " = ((saddr_t)" + src + " < " + from_imm(instr.Itype.signed_imm()) + ") ? 1 : 0;");
				break;
			case 0x3: // SLTU:
				add_code(
					dst + " = (" + src + " < (addr_t) " + from_imm(instr.Itype.signed_imm()) + ") ? 1 : 0;");
				break;
			case 0x4: // XORI:
				emit_op(" ^ ", " ^= ", instr.Itype.rd, instr.Itype.rs1, from_imm(instr.Itype.signed_imm()));
				break;
			case 0x5: // SRLI / SRAI / ORC.B:
				if (instr.Itype.is_rori() && (W != 4 || (instr.Itype.imm & 0x20) == 0)) {
					// RORI: Rotate right immediate
					add_code(
					"{const unsigned shift = " + from_imm(instr.Itype.imm & (XLEN-1)) + ";\n",
						dst + " = (" + src + " >> shift) | (" + src + " << ((XLEN - shift) & (XLEN-1))); }"
					);
				} else if (instr.Itype.imm == 0x287) {
					// ORC.B: Bitwise OR-combine
					add_code(
						"for (unsigned i = 0; i < sizeof(addr_t); i++)",
						"	((int8_t *)&" + dst + ")[i] = ((int8_t *)&" + src + ")[i] ? 0xFF : 0x0;"
					);
				} else if (instr.Itype.is_rev8<sizeof(dst)>()) {
					// REV8: Byte-reverse register
					if constexpr (W == 4)
						add_code(dst + " = do_bswap32(" + src + ");");
					else
						add_code(dst + " = do_bswap64(" + src + ");");
				} else if (instr.Itype.high_bits() == 0x0 && (W != 4 || (instr.Itype.imm & 0x20) == 0)) {
					// SRLI: Logical right-shift immediate
					emit_op(" >> ", " >>= ", instr.Itype.rd, instr.Itype.rs1,
						std::to_string(instr.Itype.shift64_imm() & (XLEN-1)));
				} else if (instr.Itype.high_bits() == 0x400 && (W != 4 || (instr.Itype.imm & 0x20) == 0)) {
					// SRAI: Arithmetic right-shift immediate
					add_code(
						dst + " = (saddr_t)" + src + " >> " + std::to_string(instr.Itype.imm & (XLEN-1)) + ";");
				} else if (instr.Itype.high_bits() == 0x480 && (W != 4 || (instr.Itype.imm & 0x20) == 0)) { // BEXTI: Bit-extract immediate
					add_code(
						dst + " = (" + src + " >> (" + std::to_string(instr.Itype.imm & (XLEN-1)) + ")) & 1;");
				} else if (instr.Itype.imm == 0b011010000111) {
					add_code(dst + " = do_brev8(" + src + ");");
				} else if (W == 4 && instr.Itype.imm == 0b000010001111) {
					add_code(dst + " = do_unzip32(" + src + ");");
				} else {
					UNKNOWN_INSTRUCTION();
				}
				break;
			case 0x6: // ORI
				add_code(
					dst + " = " + src + " | " + from_imm(instr.Itype.signed_imm()) + ";");
				break;
			case 0x7: // ANDI
				add_code(
					dst + " = " + src + " & " + from_imm(instr.Itype.signed_imm()) + ";");
				break;
			default:
				UNKNOWN_INSTRUCTION();
			}
			// Register tracking (mostly ADDI)
			if (instr.Itype.funct3 == 0) {
				// Track register value when rs1 == 0:
				if (instr.Itype.rs1 == 0) {
					this->track_register_value(instr.Itype.rd, instr.Itype.signed_imm());
				} else {
					if (auto tracked_value = get_tracked_register(instr.Itype.rs1)) {
						this->track_register_value(instr.Itype.rd, instr.Itype.signed_imm() + *tracked_value);
					} else {
						this->reset_tracked_register(instr.Itype.rd);
					}
				}
			} else {
				this->reset_tracked_register(instr.Itype.rd);
			}
			} break;
		case RV32I_OP:
			if (UNLIKELY(instr.Rtype.rd == 0)) break;

			switch (instr.Rtype.jumptable_friendly_op()) {
			case 0x0: // ADD
				if (instr.Rtype.rs2 == instr.Rtype.rd) {
					// Make sure we can perform rd += rs1
					emit_op(" + ", " += ", instr.Rtype.rd, instr.Rtype.rs2, from_reg(instr.Rtype.rs1));
				} else {
					emit_op(" + ", " += ", instr.Rtype.rd, instr.Rtype.rs1, from_reg(instr.Rtype.rs2));
				}
				break;
			case 0x200: // SUB
				emit_op(" - ", " -= ", instr.Rtype.rd, instr.Rtype.rs1, from_reg(instr.Rtype.rs2));
				break;
			case 0x1: // SLL
				add_code(
					to_reg(instr.Rtype.rd) + " = " + from_reg(instr.Rtype.rs1) + " << (" + from_reg(instr.Rtype.rs2) + " & (XLEN-1));");
				break;
			case 0x2: // SLT
				add_code(
					to_reg(instr.Rtype.rd) + " = ((saddr_t)" + from_reg(instr.Rtype.rs1) + " < (saddr_t)" + from_reg(instr.Rtype.rs2) + ") ? 1 : 0;");
				break;
			case 0x3: // SLTU
				add_code(
					to_reg(instr.Rtype.rd) + " = (" + from_reg(instr.Rtype.rs1) + " < " + from_reg(instr.Rtype.rs2) + ") ? 1 : 0;");
				break;
			case 0x4: // XOR
				emit_op(" ^ ", " ^= ", instr.Rtype.rd, instr.Rtype.rs1, from_reg(instr.Rtype.rs2));
				break;
			case 0x5: // SRL
				add_code(
					to_reg(instr.Rtype.rd) + " = " + from_reg(instr.Rtype.rs1) + " >> (" + from_reg(instr.Rtype.rs2) + " & (XLEN-1));");
				break;
			case 0x205: // SRA
				add_code(
					to_reg(instr.Rtype.rd) + " = (saddr_t)" + from_reg(instr.Rtype.rs1) + " >> (" + from_reg(instr.Rtype.rs2) + " & (XLEN-1));");
				break;
			case 0x6: // OR
				emit_op(" | ", " |= ", instr.Rtype.rd, instr.Rtype.rs1, from_reg(instr.Rtype.rs2));
				break;
			case 0x7: // AND
				emit_op(" & ", " &= ", instr.Rtype.rd, instr.Rtype.rs1, from_reg(instr.Rtype.rs2));
				break;
			// extension RV32M / RV64M
			case 0x10: // MUL
				add_code(
					to_reg(instr.Rtype.rd) + " = (saddr_t)" + from_reg(instr.Rtype.rs1) + " * (saddr_t)" + from_reg(instr.Rtype.rs2) + ";");
				break;
			case 0x11: // MULH (signed x signed)
				if constexpr (W == 4) {
					add_code(
						to_reg(instr.Rtype.rd) + " = (uint64_t)((int64_t)(saddr_t)" + from_reg(instr.Rtype.rs1) + " * (int64_t)(saddr_t)" + from_reg(instr.Rtype.rs2) + ") >> 32u;");
				} else {
					add_code(
						"{ addr_t lhs = " + from_reg(instr.Rtype.rs1) + "; addr_t rhs = " + from_reg(instr.Rtype.rs2) + ";",
						"MUL128(&" + to_reg(instr.Rtype.rd) + ", lhs, rhs);",
						"if ((saddr_t)lhs < 0) " + to_reg(instr.Rtype.rd) + " -= rhs;",
						"if ((saddr_t)rhs < 0) " + to_reg(instr.Rtype.rd) + " -= lhs; }"
					);
				}
				break;
			case 0x12: // MULHSU (signed x unsigned)
				if constexpr (W == 4) {
					add_code(
						to_reg(instr.Rtype.rd) + " = (uint64_t)((int64_t)(saddr_t)" + from_reg(instr.Rtype.rs1) + " * (uint64_t)" + from_reg(instr.Rtype.rs2) + ") >> 32u;");
				} else {
					add_code(
						"{ addr_t lhs = " + from_reg(instr.Rtype.rs1) + "; addr_t rhs = " + from_reg(instr.Rtype.rs2) + ";",
						"MUL128(&" + to_reg(instr.Rtype.rd) + ", lhs, rhs);",
						"if ((saddr_t)lhs < 0) " + to_reg(instr.Rtype.rd) + " -= rhs; }"
					);
				}
				break;
			case 0x13: // MULHU (unsigned x unsigned)
				add_code(
					(W == 4) ?
					to_reg(instr.Rtype.rd) + " = ((uint64_t) " + from_reg(instr.Rtype.rs1) + " * (uint64_t)" + from_reg(instr.Rtype.rs2) + ") >> 32u;" :
					"MUL128(&" + to_reg(instr.Rtype.rd) + ", " + from_reg(instr.Rtype.rs1) + ", " + from_reg(instr.Rtype.rs2) + ");"
				);
				break;
			case 0x14: // DIV
				// Division by zero is not an exception: rd = -1
				// Signed overflow is not an exception either: rd = the dividend
				if constexpr (W == 8) {
					add_code(
						"if (LIKELY(" + from_reg(instr.Rtype.rs2) + " != 0)) {",
						"	if (LIKELY(!(" + from_reg(instr.Rtype.rs1) + " == -9223372036854775808ull && " + from_reg(instr.Rtype.rs2) + " == -1ull)))",
						"		" + to_reg(instr.Rtype.rd) + " = (int64_t)" + from_reg(instr.Rtype.rs1) + " / (int64_t)" + from_reg(instr.Rtype.rs2) + ";",
						"	else " + to_reg(instr.Rtype.rd) + " = " + from_reg(instr.Rtype.rs1) + ";",
						"} else " + to_reg(instr.Rtype.rd) + " = (addr_t)-1;");
				} else {
					add_code(
						"if (LIKELY(" + from_reg(instr.Rtype.rs2) + " != 0)) {",
						"	if (LIKELY(!(" + from_reg(instr.Rtype.rs1) + " == 2147483648 && " + from_reg(instr.Rtype.rs2) + " == 4294967295)))",
						"		" + to_reg(instr.Rtype.rd) + " = (int32_t)" + from_reg(instr.Rtype.rs1) + " / (int32_t)" + from_reg(instr.Rtype.rs2) + ";",
						"	else " + to_reg(instr.Rtype.rd) + " = " + from_reg(instr.Rtype.rs1) + ";",
						"} else " + to_reg(instr.Rtype.rd) + " = (addr_t)-1;");
				}
				break;
			case 0x15: // DIVU
				add_code(
					"if (LIKELY(" + from_reg(instr.Rtype.rs2) + " != 0))",
					to_reg(instr.Rtype.rd) + " = " + from_reg(instr.Rtype.rs1) + " / " + from_reg(instr.Rtype.rs2) + ";",
					"else " + to_reg(instr.Rtype.rd) + " = (addr_t)-1;"
				);
				break;
			case 0x16: // REM
				// Division by zero is not an exception: rd = the dividend
				// Signed overflow is not an exception either: rd = 0
				if constexpr (W == 8) {
					add_code(
					"if (LIKELY(" + from_reg(instr.Rtype.rs2) + " != 0)) {",
					"	if (LIKELY(!(" + from_reg(instr.Rtype.rs1) + " == -9223372036854775808ull && " + from_reg(instr.Rtype.rs2) + " == -1ull)))",
					"		" + to_reg(instr.Rtype.rd) + " = (int64_t)" + from_reg(instr.Rtype.rs1) + " % (int64_t)" + from_reg(instr.Rtype.rs2) + ";",
					"	else " + to_reg(instr.Rtype.rd) + " = 0;",
					"} else " + to_reg(instr.Rtype.rd) + " = " + from_reg(instr.Rtype.rs1) + ";");
				} else {
					add_code(
					"if (LIKELY(" + from_reg(instr.Rtype.rs2) + " != 0)) {",
					"	if (LIKELY(!(" + from_reg(instr.Rtype.rs1) + " == 2147483648 && " + from_reg(instr.Rtype.rs2) + " == 4294967295)))",
					"		" + to_reg(instr.Rtype.rd) + " = (int32_t)" + from_reg(instr.Rtype.rs1) + " % (int32_t)" + from_reg(instr.Rtype.rs2) + ";",
					"	else " + to_reg(instr.Rtype.rd) + " = 0;",
					"} else " + to_reg(instr.Rtype.rd) + " = " + from_reg(instr.Rtype.rs1) + ";");
				}
				break;
			case 0x17: // REMU
				add_code(
				"if (LIKELY(" + from_reg(instr.Rtype.rs2) + " != 0))",
					to_reg(instr.Rtype.rd) + " = " + from_reg(instr.Rtype.rs1) + " % " + from_reg(instr.Rtype.rs2) + ";",
					"else " + to_reg(instr.Rtype.rd) + " = " + from_reg(instr.Rtype.rs1) + ";"
				);
				break;
			case 0x44: // PACK (ZEXT.H on RV32 when rs2 == 0)
				if constexpr (W == 4) {
					add_code(to_reg(instr.Rtype.rd) + " = (addr_t)(uint16_t)(" + from_reg(instr.Rtype.rs1) + ") | ((addr_t)(uint16_t)(" + from_reg(instr.Rtype.rs2) + ") << 16);");
				} else {
					add_code(to_reg(instr.Rtype.rd) + " = (addr_t)(uint32_t)(" + from_reg(instr.Rtype.rs1) + ") | ((addr_t)(uint32_t)(" + from_reg(instr.Rtype.rs2) + ") << 32);");
				}
				break;
			case 0x47: // PACKH
				add_code(to_reg(instr.Rtype.rd) + " = (addr_t)(uint8_t)(" + from_reg(instr.Rtype.rs1) + ") | ((addr_t)(uint8_t)(" + from_reg(instr.Rtype.rs2) + ") << 8);");
				break;
			case 0x51: // CLMUL
				add_code(
					"{ addr_t result = 0;",
					"for (unsigned i = 0; i < XLEN; i++)",
					"  if ((" + from_reg(instr.Rtype.rs2) + " >> i) & 1)",
					"    result ^= (" + from_reg(instr.Rtype.rs1) + " << i);",
					to_reg(instr.Rtype.rd) + " = result; }");
				break;
			case 0x52: // CLMULR
				add_code(
					"{ addr_t result = 0;",
					"for (unsigned i = 0; i < XLEN; i++)",
					"  if ((" + from_reg(instr.Rtype.rs2) + " >> i) & 1)",
					"    result ^= (" + from_reg(instr.Rtype.rs1) + " >> (XLEN - i - 1));",
					to_reg(instr.Rtype.rd) + " = result; }");
				break;
			case 0x53: // CLMULH
				add_code(
					"{ addr_t result = 0;",
					"for (unsigned i = 1; i < XLEN; i++)",
					"  if ((" + from_reg(instr.Rtype.rs2) + " >> i) & 1)",
					"    result ^= (" + from_reg(instr.Rtype.rs1) + " >> (XLEN - i));",
					to_reg(instr.Rtype.rd) + " = result; }");
				break;
			case 0x102: // SH1ADD
				add_code(to_reg(instr.Rtype.rd) + " = " + from_reg(instr.Rtype.rs2) + " + (" + from_reg(instr.Rtype.rs1) + " << 1);");
				break;
			case 0x104: // SH2ADD
				add_code(to_reg(instr.Rtype.rd) + " = " + from_reg(instr.Rtype.rs2) + " + (" + from_reg(instr.Rtype.rs1) + " << 2);");
				break;
			case 0x106: // SH3ADD
				add_code(to_reg(instr.Rtype.rd) + " = " + from_reg(instr.Rtype.rs2) + " + (" + from_reg(instr.Rtype.rs1) + " << 3);");
				break;
			case 0x141: // BSET
				add_code(to_reg(instr.Rtype.rd) + " = " + from_reg(instr.Rtype.rs1) + " | ((addr_t)1 << (" + from_reg(instr.Rtype.rs2) + " & (XLEN-1)));");
				break;
			case 0x204: // XNOR
				add_code(to_reg(instr.Rtype.rd) + " = ~(" + from_reg(instr.Rtype.rs1) + " ^ " + from_reg(instr.Rtype.rs2) + ");");
				break;
			case 0x206: // ORN
				add_code(to_reg(instr.Rtype.rd) + " = (" + from_reg(instr.Rtype.rs1) + " | ~" + from_reg(instr.Rtype.rs2) + ");");
				break;
			case 0x207: // ANDN
				add_code(to_reg(instr.Rtype.rd) + " = (" + from_reg(instr.Rtype.rs1) + " & ~" + from_reg(instr.Rtype.rs2) + ");");
				break;
			case 0x241: // BCLR
				add_code(to_reg(instr.Rtype.rd) + " = " + from_reg(instr.Rtype.rs1) + " & ~((addr_t)1 << (" + from_reg(instr.Rtype.rs2) + " & (XLEN-1)));");
				break;
			case 0x245: // BEXT
				add_code(to_reg(instr.Rtype.rd) + " = (" + from_reg(instr.Rtype.rs1) + " >> (" + from_reg(instr.Rtype.rs2) + " & (XLEN-1))) & 1;");
				break;
			case 0x54: // MIN
				add_code(to_reg(instr.Rtype.rd) + " = ((saddr_t)" + from_reg(instr.Rtype.rs1) + " < (saddr_t)" + from_reg(instr.Rtype.rs2) + ") "
					" ? " + from_reg(instr.Rtype.rs1) + " : " + from_reg(instr.Rtype.rs2) + ";");
				break;
			case 0x55: // MINU
				add_code(to_reg(instr.Rtype.rd) + " = (" + from_reg(instr.Rtype.rs1) + " < " + from_reg(instr.Rtype.rs2) + ") "
					" ? " + from_reg(instr.Rtype.rs1) + " : " + from_reg(instr.Rtype.rs2) + ";");
				break;
			case 0x56: // MAX
				add_code(to_reg(instr.Rtype.rd) + " = ((saddr_t)" + from_reg(instr.Rtype.rs1) + " > (saddr_t)" + from_reg(instr.Rtype.rs2) + ") "
					" ? " + from_reg(instr.Rtype.rs1) + " : " + from_reg(instr.Rtype.rs2) + ";");
				break;
			case 0x57: // MAXU
				add_code(to_reg(instr.Rtype.rd) + " = (" + from_reg(instr.Rtype.rs1) + " > " + from_reg(instr.Rtype.rs2) + ") "
					" ? " + from_reg(instr.Rtype.rs1) + " : " + from_reg(instr.Rtype.rs2) + ";");
				break;
			case 0x75: // CZERO.EQZ
				// dst = (src2 == 0) ? 0 : src1;
				add_code(to_reg(instr.Rtype.rd) + " = (" + from_reg(instr.Rtype.rs2) + " == 0) ? 0 : " + from_reg(instr.Rtype.rs1) + ";");
				break;
			case 0x77: // CZERO.NEZ
				// dst = (src2 != 0) ? 0 : src1;
				add_code(to_reg(instr.Rtype.rd) + " = (" + from_reg(instr.Rtype.rs2) + " != 0) ? 0 : " + from_reg(instr.Rtype.rs1) + ";");
				break;
			// The complementary shift count is masked, as a zero rotate would
			// otherwise shift by the full register width
			case 0x301: // ROL: Rotate left
				add_code(
				"{const unsigned shift = " + from_reg(instr.Rtype.rs2) + " & (XLEN-1);\n",
					to_reg(instr.Rtype.rd) + " = (" + from_reg(instr.Rtype.rs1) + " << shift) | (" + from_reg(instr.Rtype.rs1) + " >> ((XLEN - shift) & (XLEN-1))); }"
				);
				break;
			case 0x305: // ROR: Rotate right
				add_code(
				"{const unsigned shift = " + from_reg(instr.Rtype.rs2) + " & (XLEN-1);\n",
					to_reg(instr.Rtype.rd) + " = (" + from_reg(instr.Rtype.rs1) + " >> shift) | (" + from_reg(instr.Rtype.rs1) + " << ((XLEN - shift) & (XLEN-1))); }"
				);
				break;
			case 0x341: // BINV
				add_code(to_reg(instr.Rtype.rd) + " = " + from_reg(instr.Rtype.rs1) + " ^ ((addr_t)1 << (" + from_reg(instr.Rtype.rs2) + " & (XLEN-1)));");
				break;
			default:
				//fprintf(stderr, "RV32I_OP: Unhandled function 0x%X\n",
				//		instr.Rtype.jumptable_friendly_op());
				UNKNOWN_INSTRUCTION();
			}
			this->reset_tracked_register(instr.Rtype.rd);
			break;
		case RV32I_LUI:
			if (UNLIKELY(instr.Utype.rd == 0))
				break;
			add_code(
				to_reg(instr.Utype.rd) + " = " + from_imm(instr.Utype.upper_imm()) + ";");
			this->track_register_value(instr.Utype.rd, instr.Utype.upper_imm());
			break;
		case RV32I_AUIPC:
			if (UNLIKELY(instr.Utype.rd == 0))
				break;
			add_code(
				to_reg(instr.Utype.rd) + " = " + PCRELS(instr.Utype.upper_imm()) + ";");
			this->track_register_value(instr.Utype.rd, PCRELA(instr.Utype.upper_imm()));
			break;
		case RV32I_FENCE:
			if (instr.Itype.funct3 == 0x1) {
				WELL_KNOWN_INSTRUCTION();
				exit_function(PCRELS(4), false);
				this->add_reentry_next();
			} else if (instr.Itype.funct3 != 0x0) {
				UNKNOWN_INSTRUCTION();
			}
			break;
		case RV32I_SYSTEM:
			if (instr.Itype.funct3 == 0x0) {
				this->increment_counter_so_far();
				// System calls and EBREAK
				if (instr.Itype.imm < 2) {
					std::string syscall_reg;
					if (instr.Itype.imm == 0) {
						// ECALL: System call
						syscall_reg = this->from_reg(REG_ECALL);
						this->emit_system_call(syscall_reg, false);
					} else { // EBREAK
						syscall_reg = std::to_string(SYSCALL_EBREAK);
						this->emit_system_call(syscall_reg, true);
					}
					break;
				} else if (instr.Itype.imm == 261 || instr.Itype.imm == 0x7FF) { // WFI / STOP
					code += "max_ic = 0;\n"; // Immediate stop PC + 4
					exit_function(PCRELS(4), false);
					this->add_reentry_next();
					break;
				} else {
					this->load_register(instr.Itype.rd);
					this->potentially_realize_register(instr.Itype.rd);
					this->load_register(instr.Itype.rs1);
					this->potentially_realize_register(instr.Itype.rs1);
					// Zero funct3, unknown imm: Don't exit
					// The system handler is opaque and may write any register
					this->invalidate_all_bounds_checks();
					code += "cpu->pc = " + PCRELS(0) + ";\n";
					if (tinfo.is_libtcc) {
						code += "if (api.system(cpu, " + std::to_string(instr.whole) +"))\n";
						code += "  RETURN_VALUES(0, 0);\n";
					} else {
						code += "api.system(cpu, " + std::to_string(instr.whole) +");\n";
					}
					this->reset_tracked_register(instr.Itype.rd);
					this->potentially_reload_register(instr.Itype.rd);
					break;
				}
			} else {
				// Non-zero funct3: CSR and other system functions
				this->load_register(instr.Itype.rd);
				this->potentially_realize_register(instr.Itype.rd);
				this->load_register(instr.Itype.rs1);
				this->potentially_realize_register(instr.Itype.rs1);
				// The system handler is opaque and may write any register
				this->invalidate_all_bounds_checks();
				code += "cpu->pc = " + PCRELS(0) + ";\n";
				if (!tinfo.ignore_instruction_limit)
					code += "INS_COUNTER(cpu) = ic;\n"; // Reveal instruction counters
				code += "MAX_COUNTER(cpu) = max_ic;\n";
				if (tinfo.is_libtcc) {
					code += "if (api.system(cpu, " + std::to_string(instr.whole) +"))\n";
					code += "  RETURN_VALUES(0, 0);\n";
				} else {
					code += "api.system(cpu, " + std::to_string(instr.whole) +");\n";
				}
				this->reset_tracked_register(instr.Itype.rd);
				this->potentially_reload_register(instr.Itype.rd);
			} break;
		case RV64I_OP_IMM32: {
			if constexpr (W < 8) {
				UNKNOWN_INSTRUCTION();
				break;
			}
			if (UNLIKELY(instr.Itype.rd == 0))
				break;
			const auto dst = to_reg(instr.Itype.rd);
			const auto src = "(uint32_t)" + from_reg(instr.Itype.rs1);
			if (instr.Itype.funct3 == 0x0) {
				// Track register value when rs1 == 0:
				if (instr.Itype.rs1 == 0) {
					this->track_register_value(instr.Itype.rd, (int32_t)instr.Itype.signed_imm());
				} else {
					this->reset_tracked_register(instr.Itype.rd);
				}
			} else {
				this->reset_tracked_register(instr.Itype.rd);
			}
			switch (instr.Itype.funct3) {
			case 0x0:
				// ADDIW: Add sign-extended 12-bit immediate
				add_code(dst + " = " + SIGNEXTW + " (" + src + " + " + from_imm(instr.Itype.signed_imm()) + ");");
				break;
			case 0x1: // SLLI.W / SLLI.UW:
				if (instr.Itype.high_bits() == 0x000 && (instr.Itype.imm & 0x20) == 0) {
					add_code(dst + " = " + SIGNEXTW + " (" + src + " << " + from_imm(instr.Itype.shift_imm()) + ");");
				} else if (instr.Itype.high_bits() == 0x080) {
					// SLLI.UW (full 6-bit RV64 shamt)
					add_code(dst + " = ((addr_t)" + src + " << " + from_imm(instr.Itype.shift64_imm()) + ");");
				} else {
					switch (instr.Itype.imm) {
					case 0b011000000000: // CLZ.W
						add_code(dst + " = " + src + " ? do_clz(" + src + ") : 32;");
						break;
					case 0b011000000001: // CTZ.W
						add_code(dst + " = " + src + " ? do_ctz(" + src + ") : 32;");
						break;
					case 0b011000000010: // CPOP.W
						add_code(dst + " = do_cpop(" + src + ");");
						break;
					default:
						UNKNOWN_INSTRUCTION();
					}
				}
				break;
			case 0x5: // SRLIW / SRAIW:
				if (instr.Itype.high_bits() == 0x0 && (instr.Itype.imm & 0x20) == 0) { // SRLIW
					add_code(dst + " = " + SIGNEXTW + " (" + src + " >> " + from_imm(instr.Itype.shift_imm()) + ");");
				} else if (instr.Itype.high_bits() == 0x400 && (instr.Itype.imm & 0x20) == 0) { // SRAIW: preserve the sign bit
					add_code(
						dst + " = (int32_t)" + src + " >> " + from_imm(instr.Itype.shift_imm()) + ";");
				} else if (instr.Itype.high_bits() == 0x600 && (instr.Itype.imm & 0x20) == 0) { // RORIW
					add_code(
						"{const unsigned shift = " + from_imm(instr.Itype.imm) + " & 31;\n",
						"const uint32_t word = " + src + ";\n",
						dst + " = (int32_t)((word >> shift) | (word << ((32 - shift) & 31))); }"
					);
				} else {
					UNKNOWN_INSTRUCTION();
				}
				break;
			default:
				UNKNOWN_INSTRUCTION();
			}
			} break;
		case RV64I_OP32: {
			if constexpr (W < 8) {
				UNKNOWN_INSTRUCTION();
				break;
			}
			if (UNLIKELY(instr.Rtype.rd == 0))
				break;
			const auto dst = to_reg(instr.Rtype.rd);
			const auto src1 = "(uint32_t)" + from_reg(instr.Rtype.rs1);
			const auto src2 = "(uint32_t)" + from_reg(instr.Rtype.rs2);

			switch (instr.Rtype.jumptable_friendly_op()) {
			case 0x0: // ADDW
				add_code(dst + " = " + SIGNEXTW + " (" + src1 + " + " + src2 + ");");
				break;
			case 0x200: // SUBW
				add_code(dst + " = " + SIGNEXTW + " (" + src1 + " - " + src2 + ");");
				break;
			case 0x1: // SLLW
				add_code(dst + " = " + SIGNEXTW + " (" + src1 + " << (" + src2 + " & 0x1F));");
				break;
			case 0x5: // SRLW
				add_code(dst + " = " + SIGNEXTW + " (" + src1 + " >> (" + src2 + " & 0x1F));");
				break;
			case 0x205: // SRAW
				add_code(dst + " = (int32_t)" + src1 + " >> (" + src2 + " & 31);");
				break;
			// M-extension
			case 0x10: // MULW
				add_code(dst + " = " + SIGNEXTW + "(" + src1 + " * " + src2 + ");");
				break;
			case 0x14: // DIVW
				// Division by zero is not an exception: rd = -1
				// Signed overflow is not an exception either: rd = the dividend
				add_code(
				"if (LIKELY(" + src2 + " != 0)) {",
				"	if (LIKELY(!((int32_t)" + src1 + " == -2147483648 && (int32_t)" + src2 + " == -1)))",
				"		" + dst + " = " + SIGNEXTW + " ((int32_t)" + src1 + " / (int32_t)" + src2 + ");",
				"	else " + dst + " = " + SIGNEXTW + " (" + src1 + ");",
				"} else " + dst + " = (addr_t)(int32_t)-1;");
				break;
			case 0x15: // DIVUW
				add_code(
				"if (LIKELY(" + src2 + " != 0))",
				dst + " = " + SIGNEXTW + " (" + src1 + " / " + src2 + ");",
				"else " + dst + " = (addr_t)(int32_t)-1;");
				break;
			case 0x16: // REMW
				// Division by zero is not an exception: rd = the dividend
				// Signed overflow is not an exception either: rd = 0
				add_code(
				"if (LIKELY(" + src2 + " != 0)) {",
				"	if (LIKELY(!((int32_t)" + src1 + " == -2147483648 && (int32_t)" + src2 + " == -1)))",
				"		" + dst + " = " + SIGNEXTW + " ((int32_t)" + src1 + " % (int32_t)" + src2 + ");",
				"	else " + dst + " = 0;",
				"} else " + dst + " = " + SIGNEXTW + " (" + src1 + ");");
				break;
			case 0x17: // REMUW
				add_code(
				"if (LIKELY(" + src2 + " != 0))",
				dst + " = " + SIGNEXTW + " (" + src1 + " % " + src2 + ");",
				"else " + dst + " = " + SIGNEXTW + " (" + src1 + ");");
				break;
			case 0x40: // ADD.UW
				add_code(dst + " = " + from_reg(instr.Rtype.rs2) + " + " + src1 + ";");
				break;
			case 0x44: // ZEXT.H / PACKW
				if (instr.Rtype.rs2 == 0) {
					add_code(dst + " = (uint16_t)(" + src1 + ");");
				} else {
					add_code(dst + " = (int32_t)((uint16_t)(" + src1 + ") | ((uint32_t)(uint16_t)(" + src2 + ") << 16));");
				}
				break;
			case 0x102: // SH1ADD.UW
				add_code(dst + " = " + from_reg(instr.Rtype.rs2) + " + ((addr_t)" + src1 + " << 1);");
				break;
			case 0x104: // SH2ADD.UW
				add_code(dst + " = " + from_reg(instr.Rtype.rs2) + " + ((addr_t)" + src1 + " << 2);");
				break;
			case 0x106: // SH3ADD.UW
				add_code(dst + " = " + from_reg(instr.Rtype.rs2) + " + ((addr_t)" + src1 + " << 3);");
				break;
			case 0x301: // ROLW: Rotate left 32-bit
				add_code(
				"{const unsigned shift = " + from_reg(instr.Rtype.rs2) + " & 31;\n",
					"const uint32_t word = (uint32_t)" + from_reg(instr.Rtype.rs1) + ";\n",
					dst + " = (int32_t)((word << shift) | (word >> ((32 - shift) & 31))); }"
				);
				break;
			case 0x305: // RORW: Rotate right (32-bit)
				add_code(
				"{const unsigned shift = " + from_reg(instr.Rtype.rs2) + " & 31;\n",
					"const uint32_t word = (uint32_t)" + from_reg(instr.Rtype.rs1) + ";\n",
					dst + " = (int32_t)((word >> shift) | (word << ((32 - shift) & 31))); }"
				);
				break;
			default:
				UNKNOWN_INSTRUCTION();
			}
			this->reset_tracked_register(instr.Rtype.rd);
			} break;
		case RV32F_LOAD: {
			const rv32f_instruction fi{instr};
			switch (fi.Itype.funct3) {
			case 0x1: // FLH (Zfhmin), boxed at sixteen bits
				this->memory_load<uint16_t>(from_fpreg(fi.Itype.rd) + ".i32[0]", "uint16_t", fi.Itype.rs1, fi.Itype.signed_imm());
				if constexpr (nanboxing) {
					code += from_fpreg(fi.Itype.rd) + ".i64 |= (int64_t)0xFFFFFFFFFFFF0000ULL;\n";
				}
				break;
			case 0x2: // FLW
				this->memory_load<uint32_t>(from_fpreg(fi.Itype.rd) + ".i32[0]", "uint32_t", fi.Itype.rs1, fi.Itype.signed_imm());
				if constexpr (nanboxing) {
					code += from_fpreg(fi.Itype.rd) + ".i32[1] = ~0;\n";
				}
				break;
			case 0x3: // FLD
				this->memory_load<uint64_t>(from_fpreg(fi.Itype.rd) + ".i64", "uint64_t", fi.Itype.rs1, fi.Itype.signed_imm());
				break;
#ifdef RISCV_EXT_VECTOR
		// Vector loads. funct3 is the element width here: 0 for 8-bit and
		// 5/6/7 for 16/32/64-bit, none of which collide with the scalar
		// FLH/FLW/FLD/FLQ encodings above.
		case 0x0: case 0x5: case 0x6: case 0x7:
			// The contiguous forms -- unit-stride, whole-register and mask,
			// masked or not -- are inlined as an arena move at an address
			// the emitted code tests. Everything else goes to the handler,
			// which only reads x[rs1] and writes no integer register, so
			// cached registers stay live across it.
			this->emit_vector_instruction();
			break;
#endif
			default:
				UNKNOWN_INSTRUCTION();
				break;
			}
			} break;
		case RV32F_STORE: {
			const rv32f_instruction fi{instr};
			switch (fi.Itype.funct3) {
			case 0x1: // FSH (Zfhmin)
				this->memory_store("int16_t", sizeof(int16_t), fi.Stype.rs1, fi.Stype.signed_imm(), from_fpreg(fi.Stype.rs2) + ".i32[0]");
				break;
			case 0x2: // FSW
				this->memory_store("int32_t", sizeof(int32_t), fi.Stype.rs1, fi.Stype.signed_imm(), from_fpreg(fi.Stype.rs2) + ".i32[0]");
				break;
			case 0x3: // FSD
				this->memory_store("int64_t", sizeof(int64_t), fi.Stype.rs1, fi.Stype.signed_imm(), from_fpreg(fi.Stype.rs2) + ".i64");
				break;
#ifdef RISCV_EXT_VECTOR
		// Vector stores: like the load case above
		case 0x0: case 0x5: case 0x6: case 0x7:
			this->emit_vector_instruction();
			break;
#endif
			default:
				UNKNOWN_INSTRUCTION();
				break;
			}
			} break;
		case RV32F_FMADD:
		case RV32F_FMSUB:
		case RV32F_FNMADD:
		case RV32F_FNMSUB: {
			// RISC-V spec §11.6: FMA must round only once. Route through
			// api.fmaf{32,64} (std::fma) so the generated C is correctly
			// fused — TCC would otherwise compile `a*b+c` as two roundings.
			//   FMADD  =  rs1*rs2 + rs3 →  fma(rs1, rs2,  rs3)
			//   FMSUB  =  rs1*rs2 - rs3 →  fma(rs1, rs2, -rs3)
			//   FNMADD = -rs1*rs2 - rs3 → -fma(rs1, rs2,  rs3)
			//   FNMSUB = -rs1*rs2 + rs3 → -fma(rs1, rs2, -rs3)
			const rv32f_instruction fi{instr};
			const auto dst = from_fpreg(fi.R4type.rd);
			const auto rs1 = from_fpreg(fi.R4type.rs1);
			const auto rs2 = from_fpreg(fi.R4type.rs2);
			const auto rs3 = from_fpreg(fi.R4type.rs3);
			const bool negateResult = (instr.opcode() == RV32F_FNMADD || instr.opcode() == RV32F_FNMSUB);
			const bool subtractC    = (instr.opcode() == RV32F_FMSUB  || instr.opcode() == RV32F_FNMSUB);
			const std::string resultSign = negateResult ? "-" : "";
			const std::string cSign      = subtractC    ? "-" : "";
			if (fi.R4type.funct2 == 0x0) { // float32
				if constexpr (nanboxing && W == 8) {
					code += "if ((uint32_t)" + rs1 + ".i32[1] != 0xFFFFFFFFu || (uint32_t)" + rs2 + ".i32[1] != 0xFFFFFFFFu"
						" || (uint32_t)" + rs3 + ".i32[1] != 0xFFFFFFFFu) ";
					code += "load_fl(&" + dst + ", 0x7FC00000u);\nelse ";
				}
				code += "set_fl(&" + dst + ", " + resultSign + "api.fmaf32("
				      + rs1 + ".f32[0], " + rs2 + ".f32[0], " + cSign + rs3 + ".f32[0]));\n";
			} else if (fi.R4type.funct2 == 0x1) { // float64
				code += "set_dbl(&" + dst + ", " + resultSign + "api.fmaf64("
				      + rs1 + ".f64, " + rs2 + ".f64, " + cSign + rs3 + ".f64));\n";
			} else {
				UNKNOWN_INSTRUCTION();
			}
			} break;
		case RV32F_FPFUNC: {
			const rv32f_instruction fi{instr};
			const auto dst = from_fpreg(fi.R4type.rd);
			const auto rs1 = from_fpreg(fi.R4type.rs1);
			const auto rs2 = from_fpreg(fi.R4type.rs2);
			const bool f32 = (fi.R4type.funct2 == 0x0);
			// Operand predicates for the fcsr flag code below. A NaN has an
			// all-ones exponent and a non-zero payload; a signaling NaN also has
			// the quiet bit clear. Everything that uses these is emitted only
			// under fcsr_emulation: a scripting guest that never reads fflags
			// should not pay for the tests on every FP instruction.
			auto is_nan = [] (const std::string& r, bool is_f32) -> std::string {
				if (is_f32)
					return "((" + r + ".i32[0] & 0x7f800000u) == 0x7f800000u && (" + r + ".i32[0] & 0x007fffffu) != 0)";
				return "((" + r + ".i64 & 0x7ff0000000000000ull) == 0x7ff0000000000000ull && (" + r + ".i64 & 0x000fffffffffffffull) != 0)";
			};
			auto is_snan = [] (const std::string& r, bool is_f32) -> std::string {
				if (is_f32)
					return "((" + r + ".i32[0] & 0x7fc00000u) == 0x7f800000u && (" + r + ".i32[0] & 0x003fffffu) != 0)";
				return "((" + r + ".i64 & 0x7ff8000000000000ull) == 0x7ff0000000000000ull && (" + r + ".i64 & 0x0007ffffffffffffull) != 0)";
			};
			auto raise_nv = [] (const std::string& cond) -> std::string {
				return "if (" + cond + ") cpu->fcsr |= 0x10;\n";
			};
			// Emit `dst = <expr>`. Spec §11.3 wants a NaN result to be the
			// canonical quiet NaN rather than the payload-propagating one the
			// host FPU produces, which again is only worth a branch when the
			// guest is looking at the FCSR.
			auto emit_arith = [&] (const std::string& expr, bool is_f32) -> std::string {
				if constexpr (fcsr_emulation) {
					if (is_f32)
						return "{ const float fr = " + expr + "; if (fr != fr) load_fl(&" + dst + ", 0x7fc00000u); else set_fl(&" + dst + ", fr); }\n";
					return "{ const double dr = " + expr + "; if (dr != dr) load_dbl(&" + dst + ", 0x7ff8000000000000ull); else set_dbl(&" + dst + ", dr); }\n";
				} else {
					if (is_f32)
						return "set_fl(&" + dst + ", " + expr + ");\n";
					return "set_dbl(&" + dst + ", " + expr + ");\n";
				}
			};
			if (fi.R4type.funct2 < 0x2) { // fp32 / fp64
			switch (instr.fpfunc()) {
			case RV32F__FEQ_LT_LE:
				if (UNLIKELY(fi.R4type.rd == 0)) {
					UNKNOWN_INSTRUCTION();
					break;
				}
				// FLE/FLT are signaling compares: any NaN operand raises NV. FEQ is
				// a quiet compare and only raises NV for a signaling NaN. All of
				// them already yield 0 for unordered operands in plain C.
				// A non-NaN-boxed single-precision operand is read as the
				// canonical quiet NaN. FLE/FLT are signaling compares, so a
				// NaN operand (quiet or signaling) raises NV and the compare
				// is false; FEQ is quiet and raises NV only for sNaN. The
				// FLE/FLT NV is added in the fcsr_emulation block below via
				// the is_nan() test (a non-boxed operand is a NaN), so here
				// only the false result is forced.
				if constexpr (nanboxing && W == 8) {
					if (f32) {
						// A non-NaN-boxed operand is read as the canonical
						// quiet NaN: the compare is false and FLE/FLT raise
						// NV (signaling compare). The comparison and the
						// normal NV logic run in the else branch.
						code += "if ((uint32_t)" + rs1 + ".i32[1] != 0xFFFFFFFFu || (uint32_t)" + rs2 + ".i32[1] != 0xFFFFFFFFu) { ";
						code += to_reg(fi.R4type.rd) + " = 0;";
						if constexpr (fcsr_emulation) {
							const unsigned op = fi.R4type.funct3 | (fi.R4type.funct2 << 4);
							if (op != 0x2 && op != 0x12) // FLE/FLT only
								code += " cpu->fcsr |= 0x10;";
						}
						code += " }\nelse { ";
					}
				}
				if constexpr (fcsr_emulation) {
					const unsigned op = fi.R4type.funct3 | (fi.R4type.funct2 << 4);
					if (op == 0x2 || op == 0x12) // FEQ.S / FEQ.D
						code += raise_nv(is_snan(rs1, f32) + " || " + is_snan(rs2, f32));
					else if (op <= 0x1 || (op >= 0x10 && op <= 0x11))
						code += raise_nv(is_nan(rs1, f32) + " || " + is_nan(rs2, f32));
				}
				switch (fi.R4type.funct3 | (fi.R4type.funct2 << 4)) {
				case 0x0: // FLE.S
					code += to_reg(fi.R4type.rd) + " = (" + rs1 + ".f32[0] <= " + rs2 + ".f32[0]) ? 1 : 0;\n";
					break;
				case 0x1: // FLT.S
					code += to_reg(fi.R4type.rd) + " = (" + rs1 + ".f32[0] < " + rs2 + ".f32[0]) ? 1 : 0;\n";
					break;
				case 0x2: // FEQ.S
					code += to_reg(fi.R4type.rd) + " = (" + rs1 + ".f32[0] == " + rs2 + ".f32[0]) ? 1 : 0;\n";
					break;
				case 0x10: // FLE.D
					code += to_reg(fi.R4type.rd) + " = (" + rs1 + ".f64 <= " + rs2 + ".f64) ? 1 : 0;\n";
					break;
				case 0x11: // FLT.D
					code += to_reg(fi.R4type.rd) + " = (" + rs1 + ".f64 < " + rs2 + ".f64) ? 1 : 0;\n";
					break;
				case 0x12: // FEQ.D
					code += to_reg(fi.R4type.rd) + " = (" + rs1 + ".f64 == " + rs2 + ".f64) ? 1 : 0;\n";
					break;
				default:
					UNKNOWN_INSTRUCTION();
				}
				if constexpr (nanboxing && W == 8) {
					if (f32) code += " }\n";
				}
				this->reset_tracked_register(fi.R4type.rd);
				break;
			case RV32F__FMIN_MAX:
				// Route through api.{fmin,fmax}{32,64}_rv so the emitted
				// C honors RISC-V's -0.0 < +0.0 convention for FMIN/FMAX
				// (std::fmin/fmax leave the ±0 case implementation-
				// defined; the host's fminf/fmaxf would otherwise return
				// a sign that disagrees with the spec).
				// A signaling NaN operand raises NV; a quiet one does not. The
				// api callbacks below already return the finite operand, or the
				// canonical qNaN when both are NaN.
				// A non-NaN-boxed single-precision operand is read as the
				// canonical quiet NaN: FMIN/FMAX then return the other
				// operand, or the canonical qNaN when both are non-boxed,
				// and no NV is raised.
				if constexpr (nanboxing && W == 8) {
					if (f32) {
						code += "if ((uint32_t)" + rs1 + ".i32[1] != 0xFFFFFFFFu || (uint32_t)" + rs2 + ".i32[1] != 0xFFFFFFFFu) ";
						// Spelled as int: the JIT compiles this text as C99,
						// where <stdbool.h> is not in scope.
						code += "{ const int nb1 = (uint32_t)" + rs1 + ".i32[1] != 0xFFFFFFFFu;"
							" const int nb2 = (uint32_t)" + rs2 + ".i32[1] != 0xFFFFFFFFu;"
							" if (nb1 && nb2) load_fl(&" + dst + ", 0x7fc00000u);"
							" else if (nb1) set_fl(&" + dst + ", " + rs2 + ".f32[0]);"
							" else set_fl(&" + dst + ", " + rs1 + ".f32[0]); }\nelse ";
					}
				}
				if constexpr (fcsr_emulation) {
					const unsigned op = fi.R4type.funct3 | (fi.R4type.funct2 << 4);
					if (op <= 0x1 || (op >= 0x10 && op <= 0x11))
						code += raise_nv(is_snan(rs1, f32) + " || " + is_snan(rs2, f32));
				}
				switch (fi.R4type.funct3 | (fi.R4type.funct2 << 4)) {
				case 0x0: // FMIN.S
					code += "set_fl(&" + dst + ", api.fmin32_rv(" + rs1 + ".f32[0], " + rs2 + ".f32[0]));\n";
					break;
				case 0x1: // FMAX.S
					code += "set_fl(&" + dst + ", api.fmax32_rv(" + rs1 + ".f32[0], " + rs2 + ".f32[0]));\n";
					break;
				case 0x10: // FMIN.D
					code += "set_dbl(&" + dst + ", api.fmin64_rv(" + rs1 + ".f64, " + rs2 + ".f64));\n";
					break;
				case 0x11: // FMAX.D
					code += "set_dbl(&" + dst + ", api.fmax64_rv(" + rs1 + ".f64, " + rs2 + ".f64));\n";
					break;
				default:
					UNKNOWN_INSTRUCTION();
				} break;
			case RV32F__FADD:
			case RV32F__FSUB: {
				const std::string fop = (instr.fpfunc() == RV32F__FSUB) ? " - " : " + ";
				if (f32) {
					if constexpr (nanboxing && W == 8) {
						code += "if ((uint32_t)" + rs1 + ".i32[1] != 0xFFFFFFFFu || (uint32_t)" + rs2 + ".i32[1] != 0xFFFFFFFFu) ";
						code += "load_fl(&" + dst + ", 0x7FC00000u);\nelse ";
					}
					if constexpr (fcsr_emulation) {
						// NX when the exact (double-precision) result does not
						// round-trip to the single-precision result. The
						// operands are widened *before* the operation, which is
						// what makes the comparison meaningful; the double sum
						// is exact unless the operand exponents are more than
						// ~29 apart, so a handful of extreme cases under-report.
						code += "{ const float fa = " + rs1 + ".f32[0], fb = " + rs2 + ".f32[0];"
							" const double exact = (double)fa" + fop + "(double)fb;"
							" const float fr = fa" + fop + "fb;"
							" if (fr != fr) load_fl(&" + dst + ", 0x7fc00000u);"
							" else { set_fl(&" + dst + ", fr); if ((double)fr != exact) cpu->fcsr |= 1; } }\n";
					} else {
						code += emit_arith(rs1 + ".f32[0]" + fop + rs2 + ".f32[0]", true);
					}
				}
				else
					code += emit_arith(rs1 + ".f64" + fop + rs2 + ".f64", false);
				} break;
			case RV32F__FMUL:
				// Finite operands that produce an infinity overflowed; finite
				// operands that produce an inexact subnormal underflowed. Both
				// imply NX. The operands are read into locals first, because rd
				// is allowed to alias rs1/rs2.
				if constexpr (fcsr_emulation) {
					if (f32) {
						if constexpr (nanboxing && W == 8) {
							code += "if ((uint32_t)" + rs1 + ".i32[1] != 0xFFFFFFFFu || (uint32_t)" + rs2 + ".i32[1] != 0xFFFFFFFFu) ";
							code += "load_fl(&" + dst + ", 0x7FC00000u);\nelse ";
						}
						code += "{ const uint32_t ia = " + rs1 + ".i32[0], ib = " + rs2 + ".i32[0];"
							" const float fa = " + rs1 + ".f32[0], fb = " + rs2 + ".f32[0];"
							" const float fr = fa * fb;"
							" if (fr != fr) load_fl(&" + dst + ", 0x7fc00000u); else set_fl(&" + dst + ", fr);"
							" if ((ia & 0x7f800000u) != 0x7f800000u && (ib & 0x7f800000u) != 0x7f800000u) {"
							" const uint32_t ir = " + dst + ".i32[0] & 0x7fffffffu;"
							" if (ir == 0x7f800000u) { cpu->fcsr |= 5;"
							" const unsigned rm = (cpu->fcsr >> 5) & 7;"
							" const int neg = (" + dst + ".i32[0] & 0x80000000u) != 0;"
							" if (rm == 1 || (rm == 2 && !neg) || (rm == 3 && neg))"
							" " + dst + ".i32[0] = (" + dst + ".i32[0] & 0x80000000u) | 0x7F7FFFFFu; }"
							" else if (ir < 0x00800000u && (double)fa * (double)fb != fr) cpu->fcsr |= 3; } }\n";
						break;
					}
				}
				if constexpr (nanboxing && W == 8) {
					if (f32) {
						code += "if ((uint32_t)" + rs1 + ".i32[1] != 0xFFFFFFFFu || (uint32_t)" + rs2 + ".i32[1] != 0xFFFFFFFFu) ";
						code += "load_fl(&" + dst + ", 0x7FC00000u);\nelse ";
					}
				}
				code += emit_arith(f32 ? (rs1 + ".f32[0] * " + rs2 + ".f32[0]")
									   : (rs1 + ".f64 * " + rs2 + ".f64"), f32);
				break;
			case RV32F__FDIV:
				// DZ is only for a finite non-zero numerator over zero: 0/0 is
				// NV and inf/0 is exact. NV/NX/OF/UF are also required.
				if constexpr (fcsr_emulation) {
					if (f32) {
						if constexpr (nanboxing && W == 8) {
							code += "if ((uint32_t)" + rs1 + ".i32[1] != 0xFFFFFFFFu || (uint32_t)" + rs2 + ".i32[1] != 0xFFFFFFFFu) ";
							code += "load_fl(&" + dst + ", 0x7FC00000u);\nelse ";
						}
						code += "{ const uint32_t ia = " + rs1 + ".i32[0], ib = " + rs2 + ".i32[0];"
							" const float fa = " + rs1 + ".f32[0], fb = " + rs2 + ".f32[0];"
							" const float fr = fa / fb;"
							" if (fr != fr) { load_fl(&" + dst + ", 0x7fc00000u);"
							" if ((ia & 0x7fffffffu) == 0 && (ib & 0x7fffffffu) == 0) cpu->fcsr |= 0x10;"
							" else if ((ia & 0x7f800000u) == 0x7f800000u && (ib & 0x7f800000u) == 0x7f800000u) cpu->fcsr |= 0x10; }"
							" else { set_fl(&" + dst + ", fr);"
							" if ((ia & 0x7fffffffu) != 0 && (ia & 0x7f800000u) != 0x7f800000u"
							" && (ib & 0x7fffffffu) == 0) cpu->fcsr |= 8;"
							" if ((double)fr != (double)fa / (double)fb) cpu->fcsr |= 1;"
							" const uint32_t ir = " + dst + ".i32[0] & 0x7fffffffu;"
							" if ((ib & 0x7fffffffu) != 0 && (ia & 0x7f800000u) != 0x7f800000u && ir == 0x7f800000u) cpu->fcsr |= 5;"
							" else if (ir < 0x00800000u && (double)fr != (double)fa / (double)fb) cpu->fcsr |= 3; } }\n";
					} else {
						code += "{ const uint64_t ia = " + rs1 + ".i64, ib = " + rs2 + ".i64;"
							" const double fa = " + rs1 + ".f64, fb = " + rs2 + ".f64;"
							" const double dr = fa / fb;"
							" if (dr != dr) { load_dbl(&" + dst + ", 0x7ff8000000000000ull);"
							" if ((ia & 0x7fffffffffffffffull) == 0 && (ib & 0x7fffffffffffffffull) == 0) cpu->fcsr |= 0x10;"
							" else if ((ia & 0x7ff0000000000000ull) == 0x7ff0000000000000ull && (ib & 0x7ff0000000000000ull) == 0x7ff0000000000000ull) cpu->fcsr |= 0x10; }"
							" else { set_dbl(&" + dst + ", dr);"
							" if ((ia & 0x7fffffffffffffffull) != 0 && (ia & 0x7ff0000000000000ull) != 0x7ff0000000000000ull"
							" && (ib & 0x7fffffffffffffffull) == 0) cpu->fcsr |= 8;"
							" if ((long double)dr != (long double)fa / (long double)fb) cpu->fcsr |= 1;"
							" const uint64_t ir = " + dst + ".i64 & 0x7fffffffffffffffull;"
							" if ((ib & 0x7fffffffffffffffull) != 0 && (ia & 0x7ff0000000000000ull) != 0x7ff0000000000000ull && ir == 0x7ff0000000000000ull) cpu->fcsr |= 5;"
							" else if (ir < 0x0010000000000000ull && (long double)dr != (long double)fa / (long double)fb) cpu->fcsr |= 3; } }\n";
					}
				} else {
					if constexpr (nanboxing && W == 8) {
						if (f32) {
							code += "if ((uint32_t)" + rs1 + ".i32[1] != 0xFFFFFFFFu || (uint32_t)" + rs2 + ".i32[1] != 0xFFFFFFFFu) ";
							code += "load_fl(&" + dst + ", 0x7FC00000u);\nelse ";
						}
					}
					code += emit_arith(f32 ? (rs1 + ".f32[0] / " + rs2 + ".f32[0]")
										   : (rs1 + ".f64 / " + rs2 + ".f64"), f32);
				}
				this->penalty(f32 ? 10 : 15); // division is a slow operation
				break;
			case RV32F__FSQRT:
				// sqrt of a NaN or of a negative number is the canonical qNaN.
				// It is an invalid operation for a negative input or a signaling
				// NaN, but *not* for a quiet NaN. The invalid-flag condition is
				// evaluated before the store, since rd may alias rs1.
				if constexpr (fcsr_emulation) {
					if (f32) {
						if constexpr (nanboxing && W == 8) {
							code += "if ((uint32_t)" + rs1 + ".i32[1] != 0xFFFFFFFFu) ";
							code += "load_fl(&" + dst + ", 0x7FC00000u);\nelse ";
						}
						code += "{ const int inv = " + is_snan(rs1, true) + " || " + rs1 + ".f32[0] < 0.0f;"
							" if (" + is_nan(rs1, true) + " || " + rs1 + ".f32[0] < 0.0f)"
							" { load_fl(&" + dst + ", 0x7fc00000u); if (inv) cpu->fcsr |= 0x10; }"
							" else { const float sq = api.sqrtf32(" + rs1 + ".f32[0]); set_fl(&" + dst + ", sq);"
							" if ((double)sq * (double)sq != (double)" + rs1 + ".f32[0]) cpu->fcsr |= 1; } }\n";
					} else {
						code += "{ const int inv = " + is_snan(rs1, false) + " || " + rs1 + ".f64 < 0.0;"
							" if (" + is_nan(rs1, false) + " || " + rs1 + ".f64 < 0.0)"
							" { load_dbl(&" + dst + ", 0x7ff8000000000000ull); if (inv) cpu->fcsr |= 0x10; }"
							" else { const double sq = api.sqrtf64(" + rs1 + ".f64); set_dbl(&" + dst + ", sq);"
							" if (sq * sq != " + rs1 + ".f64) cpu->fcsr |= 1; } }\n";
					}
				} else if (f32) {
					if constexpr (nanboxing && W == 8) {
						code += "if ((uint32_t)" + rs1 + ".i32[1] != 0xFFFFFFFFu) ";
						code += "load_fl(&" + dst + ", 0x7FC00000u);\nelse ";
					}
					code += "set_fl(&" + dst + ", api.sqrtf32(" + rs1 + ".f32[0]));\n";
				} else {
					code += "set_dbl(&" + dst + ", api.sqrtf64(" + rs1 + ".f64));\n";
				}
				this->penalty(f32 ? 10 : 15); // sqrt is a slow operation
				break;
			case RV32F__FSGNJ_NX:
				// A non-NaN-boxed single-precision operand is read as the
				// canonical quiet NaN; either non-boxed source makes the
				// whole result NaN. FSGNJN additionally inverts the sign bit
				// of the NaN result (RISC-V §11.6).
				if constexpr (nanboxing && W == 8) {
					if (fi.R4type.funct2 == 0x0) {
						code += "if ((uint32_t)" + rs1 + ".i32[1] != 0xFFFFFFFFu || (uint32_t)" + rs2 + ".i32[1] != 0xFFFFFFFFu) ";
						code += "load_fl(&" + dst + ", " + (fi.R4type.funct3 == 0x1 ? "0xFFC00000u" : "0x7FC00000u") + ");\nelse ";
					}
				}
				switch (fi.R4type.funct3) {
				case 0x0: // FSGNJ
					// FMV rd, rs1
					if (fi.R4type.rs1 == fi.R4type.rs2) {
						code += dst + ".i64 = " + rs1 + ".i64;\n";
					} else {
					if (fi.R4type.funct2 == 0x0) { // fp32
						code += "load_fl(&" + dst + ", (" + rs2 + ".lsign.sign << 31) | " + rs1 + ".lsign.bits);\n";
					} else { // fp64
						code += "load_dbl(&" + dst + ", ((uint64_t)" + rs2 + ".usign.sign << 63) | " + rs1 + ".usign.bits);\n";
					} } break;
				case 0x1: // FSGNJ_N
					if (fi.R4type.funct2 == 0x0) { // fp32
						code += "load_fl(&" + dst + ", (~" + rs2 + ".lsign.sign << 31) | " + rs1 + ".lsign.bits);\n";
					} else { // fp64
						code += "load_dbl(&" + dst + ", (~(uint64_t)" + rs2 + ".usign.sign << 63) | " + rs1 + ".usign.bits);\n";
					} break;
				case 0x2: // FSGNJ_X
					if (fi.R4type.funct2 == 0x0) { // fp32
						code += "load_fl(&" + dst + ", ((" + rs1 + ".lsign.sign ^ " + rs2 + ".lsign.sign) << 31) | " + rs1 + ".lsign.bits);\n";
					} else { // fp64
						code += "load_dbl(&" + dst + ", ((uint64_t)(" + rs1 + ".usign.sign ^ " + rs2 + ".usign.sign) << 63) | " + rs1 + ".usign.bits);\n";
					} break;
				default:
					UNKNOWN_INSTRUCTION();
				} break;
			case RV32F__FCVT_SD_DS:
				// Only the single<->double pair is inlined. rs2 names the
				// source format, so anything else here is a half (Zfhmin) or
				// a quad, and goes to the handler rather than being read as
				// the format this arm happens to expect.
				if (fi.R4type.rs2 != (fi.R4type.funct2 ^ 1)) {
					UNKNOWN_INSTRUCTION();
				} else if (fi.R4type.funct2 == 0x0) {
					code += "if (" + rs1 + ".f64 != " + rs1 + ".f64) load_fl(&" + dst + ", 0x7fc00000u); else set_fl(&" + dst + ", " + rs1 + ".f64);\n";
				} else if (fi.R4type.funct2 == 0x1) {
					if constexpr (nanboxing && W == 8) {
						code += "if ((uint32_t)" + rs1 + ".i32[1] != 0xFFFFFFFFu) ";
						code += "load_dbl(&" + dst + ", 0x7ff8000000000000ull);\nelse ";
					}
					code += "if (" + rs1 + ".f32[0] != " + rs1 + ".f32[0]) load_dbl(&" + dst + ", 0x7ff8000000000000ull); else set_dbl(&" + dst + ", " + rs1 + ".f32[0]);\n";
				} else {
					UNKNOWN_INSTRUCTION();
				} break;
			case RV32F__FCVT_SD_W: {
				if (fi.R4type.funct2 < 0x2 && fi.R4type.rs2 < 0x4) {
					// FCVT.{S,D}.[LWU]
					static const std::array<const char*, 4> int_type {
						"int32_t", "uint32_t", "int64_t", "uint64_t"
					};
					const std::string itype = int_type[fi.R4type.rs2];
					const std::string value =
						"(" + itype + ")" + from_reg(fi.R4type.rs1);
					const std::string setter = f32 ? "set_fl" : "set_dbl";
					if constexpr (fcsr_emulation) {
						// NX when the destination mantissa was too narrow for the
						// integer. long double is exact for any 64-bit integer
						// where it is an 80- or 128-bit format; where it is only
						// double, the 64-bit sources just never report NX.
						// The host cast is round-to-nearest-even; the other
						// RISC-V rounding modes need a nextafter adjustment on
						// the float grid (see the interpreter counterpart).
						const std::string nafn  = f32 ? "nextafterf" : "nextafter";
						const std::string ftype = f32 ? "float" : "double";
						const std::string inf   = f32 ? "__builtin_inff()" : "__builtin_inf()";
						code += "{ const " + itype + " iv = " + value + ";"
							" const long double iex = (long double)iv;"
							" const " + ftype + " fv = (" + ftype + ")iv;"
							" const long double cld = (long double)fv;"
							" " + ftype + " out = fv;"
							" if (cld != iex) { cpu->fcsr |= 1;"
							" const unsigned rm = (" + std::to_string((int)fi.R4type.funct3) + " == 0x7) ? ((cpu->fcsr >> 5) & 7) : " + std::to_string((int)fi.R4type.funct3) + ";"
							" if (rm == 1) { if ((iex > 0.0L && cld > iex) || (iex < 0.0L && cld < iex)) out = " + nafn + "(out, (" + ftype + ")0); }"
							" else if (rm == 2) { if (cld > iex) out = " + nafn + "(out, -" + inf + "); }"
							" else if (rm == 3) { if (cld < iex) out = " + nafn + "(out, " + inf + "); }"
							// RMM differs from RNE only at an exact halfway
							// point, where it takes the larger magnitude.
							" else if (rm == 4) { if (__builtin_fabsl(cld) < __builtin_fabsl(iex)) {"
							" const " + ftype + " away = " + nafn + "(out, out < (" + ftype + ")0 ? -" + inf + " : " + inf + ");"
							" if (__builtin_fabsl((long double)away - iex) == __builtin_fabsl(iex - cld)) out = away; } } }"
							" " + setter + "(&" + dst + ", out); }\n";
					} else {
						code += setter + "(&" + dst + ", " + value + ");\n";
					}
				} else {
					UNKNOWN_INSTRUCTION();
				}
				} break;
			case RV32F__FCVT_W_SD: {
				const auto rmm = fi.R4type.funct3; // rounding mode in funct3
				if (fi.R4type.rd != 0 &&
					(fi.R4type.funct2 == 0x0 || fi.R4type.funct2 == 0x1)) {
					const bool from_float = (fi.R4type.funct2 == 0x0);
					// A non-NaN-boxed single-precision source is read as the
					// canonical quiet NaN, which converts to the maximum value
					// with NV below. FCVT must not write rs1, so the
					// substitution goes into a local copy of the register.
					const bool boxcheck = nanboxing && W == 8 && from_float;
					std::string srcdecl;
					if (boxcheck) {
						srcdecl = " fp64reg fcvt_s = " + rs1 + ";"
							" if ((uint32_t)fcvt_s.i32[1] != 0xFFFFFFFFu)"
							" fcvt_s.i32[0] = 0x7fc00000;";
					}
					const std::string src = boxcheck ? "fcvt_s.f32[0]"
						: (from_float ? (rs1 + ".f32[0]") : (rs1 + ".f64"));
					// Round per the RISC-V rounding mode (funct3), matching the
					// interpreter's fcvt_to_integer(). A bare cast is RTZ only,
					// which is wrong for e.g. RMM (std::lround) and RDN (floor).
					const char* trunc_fn = from_float ? "truncf" : "trunc"; // RTZ
					const char* floor_fn = from_float ? "floorf" : "floor"; // RDN
					const char* ceil_fn  = from_float ? "ceilf"  : "ceil";  // RUP
					const char* round_fn = from_float ? "roundf" : "round"; // RMM
					const char* near_fn  = from_float ? "nearbyintf" : "nearbyint"; // RNE
					std::string rounded;
					if (rmm == 0x7) {
						// DYN: resolve the rounding mode from the fcsr CSR (frm
						// field, bits [7:5]) at runtime. src has no side effects,
						// so it is safe to repeat across the branches.
						const std::string frm = "((cpu->fcsr >> 5) & 7)";
						rounded =
							"(" + frm + "==1?" + trunc_fn + "(" + src + "):"
								+ frm + "==2?" + floor_fn + "(" + src + "):"
								+ frm + "==3?" + ceil_fn  + "(" + src + "):"
								+ frm + "==4?" + round_fn + "(" + src + "):"
								+ near_fn + "(" + src + "))";
					} else {
						const char* rfn;
						switch (rmm) {
						case 0x1: rfn = trunc_fn; break; // RTZ
						case 0x2: rfn = floor_fn; break; // RDN
						case 0x3: rfn = ceil_fn;  break; // RUP
						case 0x4: rfn = round_fn; break; // RMM
						default:  rfn = near_fn;         // RNE
						}
						rounded = std::string(rfn) + "(" + src + ")";
					}
					// Converting an out-of-range float to an integer is undefined
					// behavior in C, and this code is handed to a C compiler, so
					// the range check is not optional. RISC-V pins the result to
					// the destination's extreme (NaN and positive overflow give
					// the maximum, negative overflow the minimum) and raises NV.
					// The bounds are powers of two, hence exactly representable.
					const char *lower = "0.0", *upper = "0.0";
					const char *maxval = "0", *minval = "0", *cast = "";
					switch (fi.R4type.rs2) {
					case 0x0: // FCVT.W (int32, sign-extended)
						lower = "-2147483648.0"; upper = "2147483648.0";
						maxval = "(int32_t)2147483647"; minval = "(int32_t)(-2147483647 - 1)";
						cast = "(int32_t)";
						break;
					case 0x1: // FCVT.WU (uint32 result, sign-extended to XLEN)
						lower = "0.0"; upper = "4294967296.0";
						maxval = "(int32_t)0xffffffffu"; minval = "(int32_t)0";
						cast = "(int32_t)(uint32_t)";
						break;
					case 0x2: // FCVT.L (int64)
						lower = "-9223372036854775808.0"; upper = "9223372036854775808.0";
						maxval = "(int64_t)9223372036854775807ll";
						minval = "(int64_t)(-9223372036854775807ll - 1)";
						cast = "(int64_t)";
						break;
					case 0x3: // FCVT.LU (uint64)
						lower = "0.0"; upper = "18446744073709551616.0";
						maxval = "(uint64_t)18446744073709551615ull"; minval = "(uint64_t)0";
						cast = "(uint64_t)";
						break;
					default: // Reserved rs2 encoding
						UNKNOWN_INSTRUCTION();
						break;
					}
					if (fi.R4type.rs2 < 0x4) {
						const std::string reg = to_reg(fi.R4type.rd);
						code += "{" + srcdecl + " const double fcvt_r = " + rounded + ";"
							" if (!(fcvt_r >= " + lower + " && fcvt_r < " + upper + ")) {"
							" " + reg + " = (fcvt_r != fcvt_r || fcvt_r >= " + upper + ") ? "
							+ maxval + " : " + minval + ";"
							+ (fcsr_emulation ? " cpu->fcsr |= 16;" : "")
							+ " } else { " + reg + " = " + cast + "fcvt_r;"
							+ (fcsr_emulation ? " if (fcvt_r != (double)(" + src + ")) cpu->fcsr |= 1;" : "")
							+ " } }\n";
					}
				} else {
					UNKNOWN_INSTRUCTION();
				}
				this->reset_tracked_register(fi.R4type.rd);
				} break;
			case RV32F__FMV_W_X:
				if (fi.R4type.rs2 != 0x0) {
					UNKNOWN_INSTRUCTION();
				} else if (fi.R4type.funct2 == 0x0) {
					code += "load_fl(&" + dst + ", " + from_reg(fi.R4type.rs1) + ");\n";
				} else if (W == 8 && fi.R4type.funct2 == 0x1) {
					code += "load_dbl(&" + dst + ", " + from_reg(fi.R4type.rs1) + ");\n";
				} else {
					UNKNOWN_INSTRUCTION();
				} break;
			case RV32F__FMV_X_W:
				if (fi.R4type.funct3 == 0x0 && fi.R4type.rs2 == 0x0) {
					if (fi.R4type.rd != 0 && fi.R4type.funct2 == 0x0) {
						code += to_reg(fi.R4type.rd) + " = " + rs1 + ".i32[0];\n";
					} else if (W == 8 && fi.R4type.rd != 0 && fi.R4type.funct2 == 0x1) { // 64-bit only
						code += to_reg(fi.R4type.rd) + " = " + rs1 + ".i64;\n";
					} else {
						UNKNOWN_INSTRUCTION();
					}
				} else { // FPCLASSIFY etc.
					UNKNOWN_INSTRUCTION();
				}
				this->reset_tracked_register(fi.R4type.rd);
				break;
			default:
				UNKNOWN_INSTRUCTION();
				break;
			} // fpfunc
			} else {
				// A format this emitter has no arm for: half (Zfhmin) or
				// quad. The handler runs instead, and some of these -- the
				// FMV.X.H that ends every half-precision read -- write an
				// integer register, so the value the tracker believes rd
				// holds has to die with them.
				UNKNOWN_INSTRUCTION();
				this->reset_tracked_register(fi.R4type.rd);
			}
			} break; // RV32F_FPFUNC
		case RV32A_ATOMIC: // General handler for atomics
			this->penalty(20); // Atomic operations are slow
			load_register(instr.Atype.rd);
			load_register(instr.Atype.rs1);
			load_register(instr.Atype.rs2);
			this->potentially_realize_register(instr.Atype.rd);
			this->potentially_realize_register(instr.Atype.rs1);
			this->potentially_realize_register(instr.Atype.rs2);
			WELL_KNOWN_INSTRUCTION();
			this->reset_tracked_register(instr.Atype.rd);
			this->potentially_reload_register(instr.Atype.rd);
			this->potentially_reload_register(instr.Atype.rs1);
			this->potentially_reload_register(instr.Atype.rs2);
			break;
		case RV32V_OP:
			// Handlers in rvv_instr.cpp implement the full vl/vtype/mask
			// semantics. Only element-wise float arithmetic is translated
			// directly, with the handler as its runtime fallback.
#ifdef RISCV_EXT_VECTOR
			this->emit_vector_instruction();
#else
			UNKNOWN_INSTRUCTION();
#endif
			break;
		case 0b1011011: // Dynamic call custom-2 instruction
			// Assumption: Dynamic calls are like regular function calls
			// Note: This behavior can be turned off by disabling register_caching
			// Load and realize registers A0-A7
			for (unsigned i = 10; i < 18; i++) {
				this->load_register(i);
			}
			store_syscall_registers();
			WELL_KNOWN_INSTRUCTION();
			// Reload registers A0-A1
			reload_syscall_registers();
			this->reset_tracked_register(10);
			this->reset_tracked_register(11);
			break;
		default:
			UNKNOWN_INSTRUCTION();
		}
	}
	// If the function ends with an unimplemented instruction,
	// we must gracefully finish, setting new PC and incrementing IC
	this->increment_counter_so_far();
	exit_function(STRADDR(this->end_pc()));
}

template <int W>
std::vector<TransMapping<W>>
CPU<W>::emit(std::string& code, const TransInfo<W>& tinfo)
{
	Emitter<W> e(tinfo);
	e.emit();

	// Create register push and pop macros
	if (tinfo.use_register_caching) {
		code += "#define STORE_REGS_" + e.get_func() + "() \\\n";
		for (size_t reg = 1; reg < e.CACHED_REGISTERS; reg++) {
			if (e.gpr_exists_at(reg)) {
				code += "  cpu->r[" + std::to_string(reg) + "] = " + e.loaded_regname(reg) + "; \\\n";
			}
		}
		code += "  ;\n";
		code += "#define LOAD_REGS_" + e.get_func() + "() \\\n";
		for (size_t reg = 1; reg < e.CACHED_REGISTERS; reg++) {
			if (e.gpr_exists_at(reg)) {
				code += "  " + e.loaded_regname(reg) + " = cpu->r[" + std::to_string(reg) + "]; \\\n";
			}
		}
		code += "  ;\n";
		if (e.used_store_syscalls()) {
			code += "#define STORE_SYS_REGS_" + e.get_func() + "() \\\n";
			for (size_t reg = 10; reg < 18; reg++) {
				if (e.gpr_exists_at(reg)) {
					code += "  cpu->r[" + std::to_string(reg) + "] = " + e.loaded_regname(reg) + "; \\\n";
				}
			}
			code += "  ;\n";
			code += "#define STORE_NON_SYS_REGS_" + e.get_func() + "() \\\n";
			for (size_t reg = 0; reg < 10; reg++) {
				if (e.gpr_exists_at(reg)) {
					code += "  cpu->r[" + std::to_string(reg) + "] = " + e.loaded_regname(reg) + "; \\\n";
				}
			}
			for (size_t reg = 18; reg < e.CACHED_REGISTERS; reg++) {
				if (e.gpr_exists_at(reg)) {
					code += "  cpu->r[" + std::to_string(reg) + "] = " + e.loaded_regname(reg) + "; \\\n";
				}
			}
			code += "  ;\n";
		}
		code += "#define LOAD_SYS_REGS_" + e.get_func() + "() \\\n";
		for (size_t reg = 10; reg < 12; reg++) {
			if (e.gpr_exists_at(reg)) {
				code += "  " + e.loaded_regname(reg) + " = cpu->r[" + std::to_string(reg) + "]; \\\n";
			}
		}
		code += "  ;\n";
	}

	// Forward declarations
	for (const auto& entry : e.get_forward_declared()) {
		code += "static ReturnValues " + entry + "(CPU*, uint64_t, uint64_t, addr_t);\n";
	}

	// Function header
	code += "static ReturnValues " + e.get_func() + "(CPU* cpu, uint64_t ic, uint64_t max_ic, addr_t pc) {\n";
	// NOTE: Scratch shared by every exit point, see RETURN_VALUES
	code += "ReturnValues retvals;\n";
	if (e.used_fixed_store())
		code += "char* mstore;\n";

	// Function GPRs
	if (tinfo.use_register_caching) {
		for (size_t reg = 1; reg < 24; reg++) {
			if (e.gpr_exists_at(reg)) {
				code += "addr_t " + e.loaded_regname(reg) + " = cpu->r[" + std::to_string(reg) + "];\n";
			}
		}
	}

	code += e.get_func() + "_jumptbl:;\n";

#if 0 // A failed attempt at a faster dispatch
	// This code exists here purely as a "no, I've tried it and it's not faster"
	// Feel free to try to optimize it further
	const auto str_begin_pc = std::to_string(e.begin_pc()) + "UL";
	code += "if (pc < " + str_begin_pc + " || pc >= " + std::to_string(e.end_pc()) + ") goto dispatch;\n";
	code += "static void* jumptbl[] = {\n";
	size_t idx = 0;
	const size_t max_idx = e.get_mappings().size();
	for (address_type<W> pc = e.begin_pc(); pc < e.end_pc(); pc += 2) {

		if (idx < max_idx) {
			const auto& entry = e.get_mappings().at(idx);
			// Default to dispatch if no mapping
			if (entry.addr != pc) {
				code += "&&dispatch,\n";
				continue;
			}
			// Label for this jumpable address
			const auto label = funclabel<W>(e.get_func(), pc);
			code += "&&" + label + ",\n";
			idx++;
		} else {
			code += "&&dispatch,\n";
		}
	}
	code += "};\n";
	code += "goto *jumptbl[(pc - " + str_begin_pc + ") >> 1];\n";
	code += "dispatch: {\n";
#else
	code += "switch (pc) {\n";
	for (size_t idx = 0; idx < e.get_mappings().size(); idx++) {
		auto& entry = e.get_mappings().at(idx);
		const auto label = funclabel<W>(e.get_func(), entry.addr);
		code += "case " + hex_address(entry.addr) + ": goto " + label + ";\n";
	}
	code += "default:\n";
#endif
	code += "exception_is_handled:\n"; // Re-using exit point for exceptions
	for (size_t reg = 1; reg < e.CACHED_REGISTERS; reg++) {
		if (e.gpr_exists_at(reg)) {
			code += "  cpu->r[" + std::to_string(reg) + "] = " + e.loaded_regname(reg) + ";\n";
		}
	}
	code += "  cpu->pc = pc; RETURN_VALUES(ic, max_ic);\n";
	code += "}\n";

	// Function code
	code += e.get_code();

	// Exception handler
	if (!tinfo.use_virtual_paging_fallback) {
		code += "exception:\n api.exception(cpu, pc, 2); max_ic = 0; goto exception_is_handled;\n";
	}
	code += "}\n";

	return std::move(e.get_mappings());
}

#ifdef RISCV_32I
template std::vector<TransMapping<4>> CPU<4>::emit(std::string&, const TransInfo<4>&);
#endif
#ifdef RISCV_64I
template std::vector<TransMapping<8>> CPU<8>::emit(std::string&, const TransInfo<8>&);
#endif
#ifdef RISCV_128I
template std::vector<TransMapping<16>> CPU<16>::emit(std::string&, const TransInfo<16>&);
#endif
} // riscv
