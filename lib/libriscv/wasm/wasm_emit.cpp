// Emit one WebAssembly function per translation block. The host engine
// compiles the module and registers its exports in Emscripten's function table.
#include "../machine.hpp"
#include "../decoder_cache.hpp"
#include "../instruction_list.hpp"
#include "../dyncall.hpp"
#include "../rv32i_instr.hpp"
#include "../rvfd.hpp"
#include "../tr_types.hpp"
#include "wasm_writer.hpp"
#include "../asmjit/aj_decode.hpp" // RVC expansion (standalone, no asmjit needed)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace riscv
{

using namespace riscv::wasm;

// Byte offsets from the CPU pointer
struct WasmLayout
{
	uint32_t regs_off;    // x0
	uint32_t pc_off;
	uint32_t fregs_off;   // f0
	uint32_t arena_off;   // the arena pointer
	uint32_t counter_off; // Machine::m_counter, followed by m_max_counter
};

// Function indices in the module import section
enum WasmImport : uint32_t {
	IMP_LOAD,     // uint64_t (CPU*, uint64_t addr, uint32_t funct3, uint64_t pc)
	IMP_STORE,    // void (CPU*, uint64_t addr, uint64_t value, uint32_t funct3, uint64_t pc)
	IMP_SYSCALL,  // uint32_t (CPU*, uint64_t pc, uint64_t ic, uint64_t max, uint64_t sysno)
	IMP_EXECUTE,  // uint32_t (CPU*, uint32_t instr, uint32_t handler, uint64_t pc, uint64_t ic, uint64_t max)
	IMP_EXECUTE_SIMPLE, // void (CPU*, uint32_t instr, uint32_t handler, uint64_t pc)
	IMP_FMA32,    // float (float, float, float): std::fma, as the handlers
	IMP_FMA64,    // double (double, double, double)
	IMP_COUNT
};

enum WasmType : uint32_t {
	TYPE_BLOCK,   // (sret, cpu, ic, max_ic, pc) -> ()
	TYPE_LOAD,
	TYPE_STORE,
	TYPE_SYSCALL,
	TYPE_EXECUTE,
	TYPE_EXECUTE_SIMPLE,
	TYPE_FMA32,
	TYPE_FMA64,
	TYPE_COUNT
};

// Yield at backward branches after this many counted instructions so V8
// can enter optimized code on the next call.
static constexpr int64_t WASM_YIELD_INSTRUCTIONS = 1 << 22;

struct WasmFuncOutput
{
	Code code;
	std::vector<uint64_t> entries;  // addresses mapped to this function
	size_t fallbacks = 0;           // instructions executed by a handler
};

enum : uint32_t {
	P_SRET = 0, P_CPU = 1, P_IC = 2, P_MAX = 3, P_PC = 4,
	L_X0 = 4,   // x1..x31 are locals 5..35
	L_LIMIT = 36, L_T0, L_T1, L_T2, L_T3, L_ADDR, // i64
	L_IDX = 42, L_ARENA, L_TI,                     // i32
	L_F32 = 45,                                    // f32
	L_F64 = 46,                                    // f64
	NUM_I64_LOCALS = 31 + 6,
	NUM_I32_LOCALS = 3,
};

template <int W>
struct WasmEmitter
{
	using address_t = address_type<W>;
	static_assert(W == 8, "The WebAssembly backend is RV64 only");

	struct Instr {
		address_t pc;
		rv32i_instruction raw;    // As found in the program
		rv32i_instruction instr;  // Expanded to 32 bits (if valid)
		uint8_t length;
		bool valid;               // Expansion succeeded
	};

	enum CtlKind : uint8_t { CTL_EXIT, CTL_TRY, CTL_LOOP, CTL_LABEL, CTL_DEFAULT, CTL_IF };
	// Controls 0, 1 and 2 are the exit block, the try block and the label loop
	static constexpr size_t CTL_FIRST_LABEL = 3;

	WasmEmitter(const TransInfo<W>& ti, const WasmLayout& lay, const MachineOptions<W>& opts)
		: tinfo(ti), layout(lay), options(opts)
	{
		this->use_arena = ti.arena_ptr != 0 && opts.translation_use_arena;
		this->arena_end   = ti.arena_size;
		this->arena_roend = ti.arena_roend;
	}

	WasmFuncOutput emit();

private:
	// --- Control flow bookkeeping ---
	void push(CtlKind k) { ctl.push_back(k); }
	void pop_end() { ctl.pop_back(); c.end(); }
	uint32_t depth_to(size_t ctl_index) const { return uint32_t(ctl.size() - 1 - ctl_index); }
	uint32_t depth_exit() const { return depth_to(0); }
	uint32_t depth_loop() const { return depth_to(2); }
	// Label k was pushed as control 3 + (N-1-k): the outermost is the last label
	size_t label_ctl(size_t k) const { return CTL_FIRST_LABEL + (labels.size() - 1 - k); }

	void if_(uint8_t bt = BLOCK_VOID) { c.if_(bt); push(CTL_IF); }
	void else_() { c.else_(); }
	void end_if() { pop_end(); }

	// --- Registers ---
	void get(unsigned r) { if (r == 0) c.i64_const(0); else c.local_get(L_X0 + r); }
	void set(unsigned r) { c.local_set(L_X0 + r); }
	void store_registers() {
		for (unsigned r = 1; r < 32; r++) {
			if (written & (1u << r)) {
				c.local_get(P_CPU); c.local_get(L_X0 + r);
				c.i64_store(layout.regs_off + 8 * r);
			}
		}
	}
	void load_registers() {
		for (unsigned r = 1; r < 32; r++) {
			if (loaded & (1u << r)) {
				c.local_get(P_CPU); c.i64_load(layout.regs_off + 8 * r);
				c.local_set(L_X0 + r);
			}
		}
	}
	void load_arena() {
		if (use_arena) {
			c.local_get(P_CPU); c.i32_load(layout.arena_off); c.local_set(L_ARENA);
		}
	}
	// limit = min(ic + YIELD, max_ic): backward jumps continue while ic < limit
	void compute_limit() {
		c.local_get(P_IC); c.i64_const(WASM_YIELD_INSTRUCTIONS); c.op(I64_ADD); c.local_tee(L_T0);
		c.local_get(P_MAX);
		c.local_get(L_T0); c.local_get(P_MAX); c.op(I64_LT_U);
		c.select();
		c.local_set(L_LIMIT);
	}
	// Emulator callbacks can change registers, counters and the arena pointer.
	void reload_after_callback() {
		load_registers();
		load_arena();
		c.local_get(P_CPU); c.i64_load(layout.counter_off);     c.local_set(P_IC);
		c.local_get(P_CPU); c.i64_load(layout.counter_off + 8); c.local_set(P_MAX);
		compute_limit();
	}

	// --- Instruction counting ---
	void flush_counter() {
		if (icount > 0) {
			c.local_get(P_IC); c.i64_const(icount); c.op(I64_ADD); c.local_set(P_IC);
			icount = 0;
		}
	}

	// --- Exits and jumps ---
	void exit_to(address_t target) {
		c.i64_const(int64_t(target)); c.local_set(P_PC); c.br(depth_exit());
	}
	// idx = (pc - base) < size ? (pc - base) >> 1 : -1, from P_PC
	void idx_from_pc() {
		c.local_get(P_PC); c.i64_const(int64_t(tinfo.basepc)); c.op(I64_SUB); c.local_tee(L_T0);
		c.i64_const(1); c.op(I64_SHR_U); c.op(I32_WRAP_I64);
		c.i32_const(-1);
		c.local_get(L_T0); c.i64_const(int64_t(tinfo.endpc - tinfo.basepc)); c.op(I64_LT_U);
		c.select();
		c.local_set(L_IDX);
	}
	int label_of(address_t addr) const {
		auto it = label_index.find(addr);
		return (it != label_index.end()) ? it->second : -1;
	}
	// Conditional jumps consume the condition from the operand stack.
	void jump(address_t target, bool conditional) {
		const int k = label_of(target);
		if (k >= 0 && k > current_label) {
			// A forward branch exits the enclosing block at the target label.
			if (conditional) c.br_if(depth_to(label_ctl(k)));
			else c.br(depth_to(label_ctl(k)));
			return;
		}
		if (conditional) if_();
		if (k >= 0) {
			c.local_get(P_IC); c.local_get(L_LIMIT); c.op(I64_LT_U);
			if_();
				c.i32_const(int32_t((target - tinfo.basepc) >> 1)); c.local_set(L_IDX);
				c.br(depth_loop());
			end_if();
		}
		exit_to(target);
		if (conditional) end_if();
	}

	// --- Memory ---
	void emit_address(unsigned rs1, int32_t imm) {
		get(rs1);
		if (imm != 0) { c.i64_const(imm); c.op(I64_ADD); }
		c.local_set(L_ADDR);
	}
	void host_address() {
		c.local_get(L_ARENA);
		c.local_get(L_ADDR);
		if constexpr (encompassing_Nbit_arena != 0 && encompassing_Nbit_arena < 32) {
			c.i64_const(int64_t(encompassing_arena_mask)); c.op(I64_AND);
		}
		c.op(I32_WRAP_I64);
		c.op(I32_ADD);
	}
	void load_op(unsigned funct3) {
		switch (funct3) {
		case 0: c.i64_load8_s(0); break;
		case 1: c.i64_load16_s(0); break;
		case 2: c.i64_load32_s(0); break;
		case 3: c.i64_load(0); break;
		case 4: c.i64_load8_u(0); break;
		case 5: c.i64_load16_u(0); break;
		case 6: c.i64_load32_u(0); break;
		}
	}
	void store_op(unsigned funct3) {
		switch (funct3) {
		case 0: c.i64_store8(0); break;
		case 1: c.i64_store16(0); break;
		case 2: c.i64_store32(0); break;
		case 3: c.i64_store(0); break;
		}
	}
	void memory_load(unsigned funct3, unsigned rs1, int32_t imm) {
		emit_address(rs1, imm);
		if (use_arena && encompassing_Nbit_arena != 0) {
			host_address(); load_op(funct3);
			return;
		}
		if (use_arena) {
			// ARENA_READABLE: (addr - 0x1000) < (END - 0x1000), see tr_api.cpp
			c.local_get(L_ADDR); c.i64_const(0x1000); c.op(I64_SUB);
			c.i64_const(int64_t(arena_end - 0x1000)); c.op(I64_LT_U);
			if_(I64);
				host_address(); load_op(funct3);
			else_();
		}
		c.local_get(P_CPU); c.local_get(L_ADDR); c.i32_const(funct3);
		c.i64_const(int64_t(pc)); c.call(IMP_LOAD);
		if (use_arena) end_if();
	}
	void memory_store(unsigned funct3, unsigned rs1, int32_t imm, unsigned rs2) {
		memory_store(funct3, rs1, imm, [this, rs2] { get(rs2); });
	}
	// value() pushes the (i64) value to store
	template <typename F>
	void memory_store(unsigned funct3, unsigned rs1, int32_t imm, F value) {
		emit_address(rs1, imm);
		if (use_arena && encompassing_Nbit_arena != 0) {
			host_address(); value(); store_op(funct3);
			return;
		}
		if (use_arena) {
			// ARENA_WRITABLE: (addr - ROEND) < (END - ROEND)
			c.local_get(L_ADDR); c.i64_const(int64_t(arena_roend)); c.op(I64_SUB);
			c.i64_const(int64_t(arena_end - arena_roend)); c.op(I64_LT_U);
			if_();
				host_address(); value(); store_op(funct3);
			else_();
		}
		c.local_get(P_CPU); c.local_get(L_ADDR); value(); c.i32_const(funct3);
		c.i64_const(int64_t(pc)); c.call(IMP_STORE);
		if (use_arena) end_if();
	}

	// Floating-point registers stay in the register file.
	uint32_t freg(unsigned r) const { return layout.fregs_off + 8 * r; }
	bool emit_float(const Instr& in);
	void emit_fcvt_to_int(unsigned rd, unsigned rs1, unsigned fmt, unsigned type, unsigned rm);

	// --- Calls into the emulator ---
	void fallback(rv32i_instruction raw) {
		if (getenv("RVWASM_STATS")) {
			const rv32i_instruction x { raw };
			if (x.length() == 2)
				printf("wasm fallback: C %04x\n", x.half[0]);
			else
				printf("wasm fallback: op=%02x f3=%x f7=%02x imm=%03x\n", x.opcode(), x.Rtype.funct3, x.Rtype.funct7, x.Itype.imm);
		}
		flush_counter();
		store_registers();
		const auto handler = CPU<W>::decode(raw).handler;
		const uint32_t hidx = DecoderData<W>::handler_index_for(handler);
		c.local_get(P_CPU); c.i32_const(int32_t(raw.whole)); c.i32_const(int32_t(hidx));
		c.i64_const(int64_t(pc)); c.local_get(P_IC); c.local_get(P_MAX);
		c.call(IMP_EXECUTE);
		c.local_set(L_TI);
		reload_after_callback();
		c.local_get(L_TI);
		if_();
			c.local_get(P_CPU); c.i64_load(layout.pc_off); c.local_set(P_PC);
			c.br(depth_exit());
		end_if();
		out.fallbacks++;
	}
	// Sync only the handler's input and output registers. This path requires
	// a handler that ignores the instruction counter and cannot stop the machine.
	void fallback_simple(rv32i_instruction raw, uint32_t reads, uint32_t writes) {
		for (unsigned r = 1; r < 32; r++) {
			if (reads & written & (1u << r)) {
				c.local_get(P_CPU); c.local_get(L_X0 + r); c.i64_store(layout.regs_off + 8 * r);
			}
		}
		const auto handler = CPU<W>::decode(raw).handler;
		const uint32_t hidx = DecoderData<W>::handler_index_for(handler);
		c.local_get(P_CPU); c.i32_const(int32_t(raw.whole)); c.i32_const(int32_t(hidx));
		c.i64_const(int64_t(pc));
		c.call(IMP_EXECUTE_SIMPLE);
		for (unsigned r = 1; r < 32; r++) {
			if (writes & loaded & (1u << r)) {
				c.local_get(P_CPU); c.i64_load(layout.regs_off + 8 * r); c.local_set(L_X0 + r);
			}
		}
		out.fallbacks++;
	}
	bool try_fallback_simple(const Instr& in);

	void system_call(bool ebreak) {
		flush_counter();
		store_registers();
		c.local_get(P_CPU); c.i64_const(int64_t(pc)); c.local_get(P_IC); c.local_get(P_MAX);
		if (ebreak) c.i64_const(SYSCALL_EBREAK);
		else get(REG_ECALL);
		c.call(IMP_SYSCALL);
		c.local_set(L_TI);
		// A system call may change any register, or switch to another thread
		reload_after_callback();
		c.local_get(L_TI);
		if_();
			c.local_get(P_CPU); c.i64_load(layout.pc_off); c.local_set(P_PC);
			c.br(depth_exit());
		end_if();
	}

	// --- Arithmetic helpers ---
	void bin64(unsigned rd, unsigned rs1, unsigned rs2, uint8_t op) {
		get(rs1); get(rs2); c.op(op); set(rd);
	}
	void bin32(unsigned rd, unsigned rs1, unsigned rs2, uint8_t op) {
		get(rs1); c.op(I32_WRAP_I64); get(rs2); c.op(I32_WRAP_I64);
		c.op(op); c.op(I64_EXTEND_I32_S); set(rd);
	}
	void zext32(unsigned r) { get(r); c.i64_const(0xFFFFFFFF); c.op(I64_AND); }
	// (rs1 op rs2) as i32 -> 0/1 in rd
	void compare64(unsigned rd, unsigned rs1, unsigned rs2, uint8_t op) {
		get(rs1); get(rs2); c.op(op); c.op(I64_EXTEND_I32_U); set(rd);
	}
	void emit_mulh(unsigned rd, unsigned rs1, unsigned rs2, unsigned funct3);
	void emit_divrem(unsigned rd, unsigned rs1, unsigned rs2, unsigned funct3, bool word);
	void select_minmax(unsigned rd, unsigned rs1, unsigned rs2, uint8_t cmp) {
		get(rs1); get(rs2); get(rs1); get(rs2); c.op(cmp); c.select(); set(rd);
	}

	bool emit_op_imm(const rv32i_instruction instr);
	bool emit_op(const rv32i_instruction instr);
	bool emit_op_imm32(const rv32i_instruction instr);
	bool emit_op32(const rv32i_instruction instr);
	void emit_instruction(const Instr& in, size_t index);
	void scan();

	const TransInfo<W>& tinfo;
	const WasmLayout& layout;
	const MachineOptions<W>& options;
	bool use_arena = false;
	address_t arena_end = 0;
	address_t arena_roend = 0;

	std::vector<Instr> instrs;
	std::vector<address_t> labels;            // sorted
	std::unordered_map<address_t, int> label_index;
	std::vector<address_t> entries;
	uint32_t loaded  = 0; // registers loaded at entry and after callbacks
	uint32_t written = 0; // registers stored at exits and before callbacks

	Code& c = out.code;
	WasmFuncOutput out;
	std::vector<CtlKind> ctl;
	int current_label = -1;
	bool previous_zero = false;
	address_t pc = 0;
	uint64_t icount = 0;
};

template <int W>
void WasmEmitter<W>::scan()
{
	address_t addr = tinfo.basepc;
	std::unordered_set<address_t> boundaries;
	for (const auto raw : tinfo.instr) {
		Instr in;
		in.pc = addr;
		in.raw = raw;
		in.length = raw.length();
		if (in.length == 2) {
#ifdef RISCV_EXT_C
			const auto d = aj_expand_rvc<W>(raw.half[0]);
			in.instr = d.instr;
			in.valid = d.valid;
#else
			in.valid = false;
#endif
		} else {
			in.instr = raw;
			in.valid = true;
		}
		boundaries.insert(addr);
		instrs.push_back(in);
		addr += in.length;
	}

	std::set<address_t> label_set;
	const auto within = [&] (address_t a) {
		return a >= tinfo.basepc && a < tinfo.endpc && boundaries.count(a);
	};
	label_set.insert(tinfo.basepc);
	for (const auto a : tinfo.global_jump_locations)
		if (within(a)) label_set.insert(a);
	for (const auto a : tinfo.jump_locations)
		if (within(a)) label_set.insert(a);

	const auto reg_bit = [] (unsigned r) -> uint32_t { return (r != 0) ? (1u << r) : 0; };
	for (const auto& in : instrs) {
		if (!in.valid) continue;
		const auto instr = in.instr;
		const address_t next = in.pc + in.length;
		switch (instr.opcode()) {
		case RV32I_BRANCH: {
			const address_t t = in.pc + instr.Btype.signed_imm();
			if (within(t)) label_set.insert(t);
			loaded |= reg_bit(instr.Btype.rs1) | reg_bit(instr.Btype.rs2);
			} break;
		case RV32I_JAL: {
			const address_t t = in.pc + instr.Jtype.jump_offset();
			if (within(t)) label_set.insert(t);
			written |= reg_bit(instr.Jtype.rd);
			if (instr.Jtype.rd == 0 && t <= in.pc && within(next)) {
				label_set.insert(next);
			}
			} break;
		case RV32I_JALR:
			loaded |= reg_bit(instr.Itype.rs1);
			written |= reg_bit(instr.Itype.rd);
			if (within(next)) label_set.insert(next);
			break;
		case RV32I_LOAD:
			loaded |= reg_bit(instr.Itype.rs1);
			written |= reg_bit(instr.Itype.rd);
			break;
		case RV32I_STORE:
			loaded |= reg_bit(instr.Stype.rs1) | reg_bit(instr.Stype.rs2);
			break;
		case RV32I_OP_IMM:
		case RV64I_OP_IMM32:
			loaded |= reg_bit(instr.Itype.rs1);
			written |= reg_bit(instr.Itype.rd);
			break;
		case RV32I_OP:
		case RV64I_OP32:
			loaded |= reg_bit(instr.Rtype.rs1) | reg_bit(instr.Rtype.rs2);
			written |= reg_bit(instr.Rtype.rd);
			break;
		case RV32I_LUI:
		case RV32I_AUIPC:
			written |= reg_bit(instr.Utype.rd);
			break;
		case RV32F_LOAD: // The integer registers of emit_float()
			loaded |= reg_bit(instr.Itype.rs1);
			break;
		case RV32F_STORE:
			loaded |= reg_bit(instr.Stype.rs1);
			break;
		case RV32F_FPFUNC:
			loaded |= reg_bit(instr.Rtype.rs1);
			written |= reg_bit(instr.Rtype.rd);
			break;
		case RV32I_SYSTEM:
			if (instr.Itype.funct3 == 0 && instr.Itype.imm == 0)
				loaded |= reg_bit(REG_ECALL);
			if (instr.Itype.funct3 == 0 && (instr.Itype.imm == 261 || instr.Itype.imm == 0x7FF)
				&& within(next)) {
				label_set.insert(next);
			}
			break;
		case RV32I_FENCE:
			if (instr.Itype.funct3 == 1 && within(next)) {
				label_set.insert(next);
			}
			break;
		}
	}
	// Initialize written registers so an early exit preserves their values.
	loaded |= written;

	labels.assign(label_set.begin(), label_set.end());
	for (size_t i = 0; i < labels.size(); i++)
		label_index.emplace(labels[i], int(i));
	// Map every label so dispatch can re-enter compiled code after a loop yields.
	entries.assign(labels.begin(), labels.end());
}

template <int W>
WasmFuncOutput WasmEmitter<W>::emit()
{
	this->scan();
	out.entries.assign(entries.begin(), entries.end());

	c.u32(4);
	c.u32(NUM_I64_LOCALS); c.u8(I64);
	c.u32(NUM_I32_LOCALS); c.u8(I32);
	c.u32(1); c.u8(F32);
	c.u32(1); c.u8(F64);

	load_arena();
	load_registers();
	compute_limit();
	idx_from_pc();

	c.block(); push(CTL_EXIT);
	// Write back guest registers before propagating an emulator exception.
	c.try_();  push(CTL_TRY);
	c.loop();  push(CTL_LOOP);
	for (size_t k = labels.size(); k-- > 0; ) {
		c.block(); push(CTL_LABEL);
	}
	c.block(); push(CTL_DEFAULT);
	{
		const size_t slots = (tinfo.endpc - tinfo.basepc) >> 1;
		std::vector<uint32_t> depths(slots, 0);
		for (size_t i = 0; i < slots; i++) {
			const int k = label_of(tinfo.basepc + 2 * i);
			depths[i] = (k >= 0) ? depth_to(label_ctl(k)) : 0; // 0: CTL_DEFAULT
		}
		c.local_get(L_IDX);
		c.br_table(depths, 0);
	}
	pop_end(); // CTL_DEFAULT
	// Return to dispatch if the PC does not match a label.
	c.br(depth_exit());

	for (size_t i = 0; i < instrs.size(); i++) {
		const auto& in = instrs[i];
		this->pc = in.pc;
		const int k = label_of(in.pc);
		if (k >= 0) {
			flush_counter();
			if (ctl.back() != CTL_LABEL || label_ctl(k) != ctl.size() - 1)
				throw MachineException(INVALID_PROGRAM, "wasm translator: label nesting mismatch");
			pop_end();
			current_label = k;
		}
		// An all-zero instruction traps. Skip subsequent zeros without a label.
		const bool is_zero = in.raw.half[0] == 0;
		if (is_zero && previous_zero && k < 0)
			continue;
		previous_zero = is_zero;
		icount++;
		emit_instruction(in, i);
		if (ctl.size() != CTL_FIRST_LABEL + labels.size() - (current_label + 1))
			throw MachineException(INVALID_PROGRAM, "wasm translator: unbalanced control flow");
	}
	flush_counter();
	exit_to(tinfo.endpc);
	pop_end(); // CTL_LOOP
	c.catch_all();
		store_registers();
		c.rethrow(0);
	pop_end(); // CTL_TRY
	pop_end(); // CTL_EXIT

	store_registers();
	c.local_get(P_CPU); c.local_get(P_PC); c.i64_store(layout.pc_off);
	c.local_get(P_SRET); c.local_get(P_IC);  c.i64_store(0);
	c.local_get(P_SRET); c.local_get(P_MAX); c.i64_store(8);
	c.end();
	return std::move(out);
}

template <int W>
bool WasmEmitter<W>::try_fallback_simple(const Instr& in)
{
	const auto instr = in.instr;
	const auto bit = [] (unsigned r) -> uint32_t { return (r != 0) ? (1u << r) : 0; };
	switch (instr.opcode()) {
	case RV32F_LOAD:  // FLW, FLD (and vector loads, which are not simple)
		if (instr.Itype.funct3 != 2 && instr.Itype.funct3 != 3) return false;
		fallback_simple(in.raw, bit(instr.Itype.rs1), 0);
		return true;
	case RV32F_STORE:
		if (instr.Stype.funct3 != 2 && instr.Stype.funct3 != 3) return false;
		fallback_simple(in.raw, bit(instr.Stype.rs1), 0);
		return true;
	case RV32F_FMADD:
	case RV32F_FMSUB:
	case RV32F_FNMADD:
	case RV32F_FNMSUB:
		fallback_simple(in.raw, 0, 0);
		return true;
	case RV32F_FPFUNC:
		switch (instr.Rtype.funct7 >> 2) {
		case RV32F__FEQ_LT_LE:
		case RV32F__FCVT_W_SD:
		case RV32F__FMV_X_W: // and FCLASS
			fallback_simple(in.raw, 0, bit(instr.Rtype.rd));
			return true;
		case RV32F__FCVT_SD_W:
		case RV32F__FMV_W_X:
			fallback_simple(in.raw, bit(instr.Rtype.rs1), 0);
			return true;
		case RV32F__FMVP_D_X:
			return false;
		default:
			fallback_simple(in.raw, 0, 0);
			return true;
		}
	case RV32A_ATOMIC:
		fallback_simple(in.raw, bit(instr.Atype.rs1) | bit(instr.Atype.rs2), bit(instr.Atype.rd));
		return true;
	}
	return false;
}

template <int W>
void WasmEmitter<W>::emit_instruction(const Instr& in, size_t)
{
	if (!in.valid) {
		fallback(in.raw);
		return;
	}
	const auto instr = in.instr;
	const address_t next_pc = in.pc + in.length;

	switch (instr.opcode()) {
	case RV32I_LOAD:
		if (instr.Itype.funct3 > 6) { fallback(in.raw); return; }
		// Match LOAD_X_DUMMY by skipping LBU, LHU and LWU to x0.
		if (instr.Itype.rd == 0 && instr.Itype.funct3 >= 4) return;
		memory_load(instr.Itype.funct3, instr.Itype.rs1, instr.Itype.signed_imm());
		if (instr.Itype.rd != 0) set(instr.Itype.rd);
		else c.drop();
		return;
	case RV32I_STORE:
		if (instr.Stype.funct3 > 3) { fallback(in.raw); return; }
		memory_store(instr.Stype.funct3, instr.Stype.rs1, instr.Stype.signed_imm(), instr.Stype.rs2);
		return;
	case RV32I_BRANCH: {
		static constexpr uint8_t ops[8] = { I64_EQ, I64_NE, 0, 0, I64_LT_S, I64_GE_S, I64_LT_U, I64_GE_U };
		const uint8_t op = ops[instr.Btype.funct3];
		const address_t target = in.pc + instr.Btype.signed_imm();
		if (op == 0 || (target & (compressed_enabled ? 0x1 : 0x3))) {
			fallback(in.raw); // Illegal, or a misaligned target (raises)
			return;
		}
		flush_counter();
		get(instr.Btype.rs1); get(instr.Btype.rs2); c.op(op);
		jump(target, true);
		return;
	}
	case RV32I_JAL: {
		const address_t target = in.pc + instr.Jtype.jump_offset();
		if (target & (compressed_enabled ? 0x1 : 0x3)) {
			fallback(in.raw);
			return;
		}
		if (instr.Jtype.rd != 0) {
			c.i64_const(int64_t(next_pc)); set(instr.Jtype.rd);
		}
		flush_counter();
		jump(target, false);
		return;
	}
	case RV32I_JALR: {
		if (instr.Itype.funct3 != 0) { fallback(in.raw); return; }
		flush_counter();
		get(instr.Itype.rs1);
		if (instr.Itype.signed_imm() != 0) { c.i64_const(instr.Itype.signed_imm()); c.op(I64_ADD); }
		c.i64_const(~int64_t(1)); c.op(I64_AND);
		c.local_set(P_PC);
		if constexpr (!compressed_enabled) {
			// Retry JALR in the interpreter to raise the alignment exception.
			c.local_get(P_PC); c.i64_const(2); c.op(I64_AND); c.op(I32_WRAP_I64);
			if_();
				exit_to(in.pc);
			end_if();
		}
		if (instr.Itype.rd != 0) {
			c.i64_const(int64_t(next_pc)); set(instr.Itype.rd);
		}
		c.local_get(P_IC); c.local_get(L_LIMIT); c.op(I64_LT_U);
		if_();
			idx_from_pc();
			c.br(depth_loop());
		end_if();
		c.br(depth_exit());
		return;
	}
	case RV32I_LUI:
		if (instr.Utype.rd != 0) {
			c.i64_const(instr.Utype.upper_imm()); set(instr.Utype.rd);
		}
		return;
	case RV32I_AUIPC:
		if (instr.Utype.rd != 0) {
			c.i64_const(int64_t(in.pc + int64_t(instr.Utype.upper_imm()))); set(instr.Utype.rd);
		}
		return;
	case RV32I_OP_IMM:
		if (instr.Itype.rd == 0) return; // NOP and HINTs
		if (emit_op_imm(instr)) return;
		break;
	case RV32I_OP:
		if (instr.Rtype.rd != 0 && emit_op(instr)) return;
		if (instr.Rtype.rd == 0 && (instr.Rtype.funct7 == 0x00 || instr.Rtype.funct7 == 0x20)) return;
		break;
	case RV64I_OP_IMM32:
		if (instr.Itype.rd != 0 && emit_op_imm32(instr)) return;
		break;
	case RV64I_OP32:
		if (instr.Rtype.rd != 0 && emit_op32(instr)) return;
		break;
	case RV32I_FENCE:
		if (instr.Itype.funct3 == 0) return; // FENCE: nothing to order
		if (instr.Itype.funct3 == 1) {
			// Return to dispatch because FENCE.I can invalidate translated code.
			fallback(in.raw);
			exit_to(next_pc);
			return;
		}
		break;
	case RV32I_SYSTEM:
		if (instr.Itype.funct3 == 0) {
			if (instr.Itype.imm == 0 || instr.Itype.imm == 1) {
				system_call(instr.Itype.imm == 1);
				return;
			}
			if (instr.Itype.imm == 261 || instr.Itype.imm == 0x7FF) {
				flush_counter();
				c.i64_const(0); c.local_set(P_MAX);
				exit_to(next_pc);
				return;
			}
		}
		break;
	case RV32F_LOAD:
	case RV32F_STORE:
	case RV32F_FMADD:
	case RV32F_FMSUB:
	case RV32F_FNMADD:
	case RV32F_FNMSUB:
	case RV32F_FPFUNC:
		if (emit_float(in)) return;
		if (try_fallback_simple(in)) return;
		break;
	default:
		if (try_fallback_simple(in)) return;
		break;
	}
	fallback(in.raw);
}

// Match rvf_instr.cpp with FCSR emulation and NaN-boxing disabled.
// Single-precision results preserve the upper 32 bits. Arithmetic rounds to
// nearest even and leaves exception flags untouched.
template <int W>
bool WasmEmitter<W>::emit_float(const Instr& in)
{
	if constexpr (fcsr_emulation || nanboxing) {
		return false;
	} else {
	const rv32f_instruction fi { in.instr };
	switch (fi.opcode()) {
	case RV32F_LOAD: {
		const unsigned f3 = fi.Itype.funct3;
		if (f3 != 2 && f3 != 3) return false; // FLW, FLD (FLH and vectors are not here)
		memory_load(f3 == 2 ? 6 : 3, fi.Itype.rs1, fi.Itype.signed_imm());
		c.local_set(L_T0);
		c.local_get(P_CPU); c.local_get(L_T0);
		if (f3 == 2) c.i64_store32(freg(fi.Itype.rd));
		else c.i64_store(freg(fi.Itype.rd));
		return true;
	}
	case RV32F_STORE: {
		const unsigned f3 = fi.Stype.funct3;
		if (f3 != 2 && f3 != 3) return false;
		const unsigned rs2 = fi.Stype.rs2;
		memory_store(f3, fi.Stype.rs1, fi.Stype.signed_imm(), [this, rs2, f3] {
			c.local_get(P_CPU);
			if (f3 == 2) c.i64_load32_u(freg(rs2));
			else c.i64_load(freg(rs2));
		});
		return true;
	}
	case RV32F_FMADD:
	case RV32F_FMSUB:
	case RV32F_FNMADD:
	case RV32F_FNMSUB: {
		// Preserve the placement of negation used by the interpreter handlers.
		const unsigned fmt = fi.R4type.funct2;
		if (fmt > 1) return false;
		const bool dbl = fmt == 1;
		const uint8_t neg = dbl ? F64_NEG : F32_NEG;
		auto fload = [&] (unsigned r) { c.local_get(P_CPU); if (dbl) c.f64_load(freg(r)); else c.f32_load(freg(r)); };
		c.local_get(P_CPU);
		fload(fi.R4type.rs1); if (fi.opcode() == RV32F_FNMSUB) c.op(neg);
		fload(fi.R4type.rs2);
		fload(fi.R4type.rs3); if (fi.opcode() == RV32F_FMSUB) c.op(neg);
		c.call(dbl ? IMP_FMA64 : IMP_FMA32);
		if (fi.opcode() == RV32F_FNMADD) c.op(neg);
		if (dbl) c.f64_store(freg(fi.R4type.rd)); else c.f32_store(freg(fi.R4type.rd));
		return true;
	}
	case RV32F_FPFUNC:
		break;
	default:
		return false;
	}
	const unsigned fmt = fi.R4type.funct2; // 0: single, 1: double
	if (fmt > 1) return false;
	const bool dbl = fmt == 1;
	const unsigned rd = fi.R4type.rd, rs1 = fi.R4type.rs1, rs2 = fi.R4type.rs2;
	const unsigned f3 = fi.R4type.funct3;
	auto fload = [&] (unsigned r) { c.local_get(P_CPU); if (dbl) c.f64_load(freg(r)); else c.f32_load(freg(r)); };
	auto fstore = [&] (unsigned r) { if (dbl) c.f64_store(freg(r)); else c.f32_store(freg(r)); };
	auto arith = [&] (uint8_t op32, uint8_t op64) {
		c.local_get(P_CPU); fload(rs1); fload(rs2); c.op(dbl ? op64 : op32); fstore(rd);
		return true;
	};
	switch (in.instr.Rtype.funct7 >> 2) {
	case RV32F__FADD: return arith(F32_ADD, F64_ADD);
	case RV32F__FSUB: return arith(F32_SUB, F64_SUB);
	case RV32F__FMUL: return arith(F32_MUL, F64_MUL);
	case RV32F__FDIV: return arith(F32_DIV, F64_DIV);
	case RV32F__FSQRT:
		if (rs2 != 0) return false;
		c.local_get(P_CPU); fload(rs1); c.op(dbl ? F64_SQRT : F32_SQRT); fstore(rd);
		return true;
	case RV32F__FSGNJ_NX: {
		if (f3 > 2) return false;
		// The magnitude of rs1 with the sign of rs2 (FSGNJ), its inverse
		// (FSGNJN) or the two signs XORed (FSGNJX), as raw bits
		c.local_get(P_CPU);
		if (dbl) {
			const int64_t sign = INT64_MIN;
			c.local_get(P_CPU); c.i64_load(freg(rs1)); c.i64_const(~sign); c.op(I64_AND);
			if (f3 == 2) { c.local_get(P_CPU); c.i64_load(freg(rs1)); c.local_get(P_CPU); c.i64_load(freg(rs2)); c.op(I64_XOR); }
			else { c.local_get(P_CPU); c.i64_load(freg(rs2)); if (f3 == 1) { c.i64_const(-1); c.op(I64_XOR); } }
			c.i64_const(sign); c.op(I64_AND);
			c.op(I64_OR);
			c.i64_store(freg(rd));
		} else {
			const int32_t sign = INT32_MIN;
			c.local_get(P_CPU); c.i32_load(freg(rs1)); c.i32_const(~sign); c.op(I32_AND);
			if (f3 == 2) { c.local_get(P_CPU); c.i32_load(freg(rs1)); c.local_get(P_CPU); c.i32_load(freg(rs2)); c.op(I32_XOR); }
			else { c.local_get(P_CPU); c.i32_load(freg(rs2)); if (f3 == 1) { c.i32_const(-1); c.op(I32_XOR); } }
			c.i32_const(sign); c.op(I32_AND);
			c.op(I32_OR);
			c.i32_store(freg(rd));
		}
		return true;
	}
	case RV32F__FEQ_LT_LE: {
		static constexpr uint8_t ops32[3] = { F32_LE, F32_LT, F32_EQ };
		static constexpr uint8_t ops64[3] = { F64_LE, F64_LT, F64_EQ };
		if (f3 > 2) return false;
		if (rd == 0) return true;
		fload(rs1); fload(rs2); c.op(dbl ? ops64[f3] : ops32[f3]);
		c.op(I64_EXTEND_I32_U); set(rd);
		return true;
	}
	case RV32F__FCVT_SD_DS: {
		// FCVT.S.D and FCVT.D.S: a NaN becomes the canonical NaN
		if (rs2 != (fmt ^ 1)) return false;
		c.local_get(P_CPU);
		if (dbl) { // FCVT.D.S
			c.local_get(P_CPU); c.f32_load(freg(rs1)); c.local_tee(L_F32);
			c.op(F64_PROMOTE_F32);
			c.i64_const(0x7ff8000000000000ll); c.op(F64_REINTERPRET_I64);
			c.local_get(L_F32); c.local_get(L_F32); c.op(F32_EQ);
			c.select();
			c.f64_store(freg(rd));
		} else { // FCVT.S.D
			c.local_get(P_CPU); c.f64_load(freg(rs1)); c.local_tee(L_F64);
			c.op(F32_DEMOTE_F64);
			c.i32_const(0x7fc00000); c.op(F32_REINTERPRET_I32);
			c.local_get(L_F64); c.local_get(L_F64); c.op(F64_EQ);
			c.select();
			c.f32_store(freg(rd));
		}
		return true;
	}
	case RV32F__FCVT_W_SD:
		if (rs2 > 3) return false;
		if (rd == 0) return true;
		// Use the interpreter for rounding modes beyond RNE, RTZ, RDN and RUP.
		if (f3 > 3) return false;
		emit_fcvt_to_int(rd, rs1, fmt, rs2, f3);
		return true;
	case RV32F__FCVT_SD_W: {
		if (rs2 > 3) return false;
		static constexpr uint8_t ops32[4] = { F32_CONVERT_I32_S, F32_CONVERT_I32_U, F32_CONVERT_I64_S, F32_CONVERT_I64_U };
		static constexpr uint8_t ops64[4] = { F64_CONVERT_I32_S, F64_CONVERT_I32_U, F64_CONVERT_I64_S, F64_CONVERT_I64_U };
		c.local_get(P_CPU);
		get(rs1);
		if (rs2 < 2) c.op(I32_WRAP_I64);
		c.op(dbl ? ops64[rs2] : ops32[rs2]);
		fstore(rd);
		return true;
	}
	case RV32F__FMV_X_W: // FMV.X.W, FMV.X.D (FCLASS is funct3 1)
		if (f3 != 0 || rs2 != 0) return false;
		if (rd == 0) return true;
		c.local_get(P_CPU);
		if (dbl) c.i64_load(freg(rs1));
		else c.i64_load32_s(freg(rs1));
		set(rd);
		return true;
	case RV32F__FMV_W_X: // FMV.W.X, FMV.D.X
		if (f3 != 0 || rs2 != 0) return false;
		c.local_get(P_CPU); get(rs1);
		if (dbl) c.i64_store(freg(rd));
		else c.i64_store32(freg(rd));
		return true;
	}
	return false;
	} // !fcsr_emulation && !nanboxing
}

// Match fcvt_to_integer() by mapping NaN to the integer maximum and
// sign-extending 32-bit results.
template <int W>
void WasmEmitter<W>::emit_fcvt_to_int(unsigned rd, unsigned rs1, unsigned fmt, unsigned type, unsigned rm)
{
	const bool dbl = fmt == 1;
	static constexpr int64_t maxval[4] = { INT32_MAX, -1 /* UINT32_MAX, sign-extended */, INT64_MAX, -1 };
	c.i64_const(maxval[type]);
	c.local_get(P_CPU);
	if (dbl) c.f64_load(freg(rs1)); else c.f32_load(freg(rs1));
	// The saturating conversion already rounds toward zero.
	static constexpr uint8_t round32[4] = { 0x90 /* f32.nearest */, 0, 0x8E /* f32.floor */, 0x8D /* f32.ceil */ };
	static constexpr uint8_t round64[4] = { 0x9E /* f64.nearest */, 0, 0x9C /* f64.floor */, 0x9B /* f64.ceil */ };
	if (rm != 1) c.op(dbl ? round64[rm] : round32[rm]);
	if (dbl) c.local_tee(L_F64); else c.local_tee(L_F32);
	static constexpr uint32_t sat32[4] = { I32_TRUNC_SAT_F32_S, I32_TRUNC_SAT_F32_U, I64_TRUNC_SAT_F32_S, I64_TRUNC_SAT_F32_U };
	static constexpr uint32_t sat64[4] = { I32_TRUNC_SAT_F64_S, I32_TRUNC_SAT_F64_U, I64_TRUNC_SAT_F64_S, I64_TRUNC_SAT_F64_U };
	c.op_fc(dbl ? sat64[type] : sat32[type]);
	if (type < 2) c.op(I64_EXTEND_I32_S);
	// select(max, converted, isnan)
	if (dbl) { c.local_get(L_F64); c.local_get(L_F64); c.op(F64_NE); }
	else { c.local_get(L_F32); c.local_get(L_F32); c.op(F32_NE); }
	c.select();
	set(rd);
}

template <int W>
bool WasmEmitter<W>::emit_op_imm(const rv32i_instruction instr)
{
	const unsigned rd = instr.Itype.rd, rs1 = instr.Itype.rs1;
	const int64_t imm = instr.Itype.signed_imm();
	const unsigned shamt = instr.Itype.shift64_imm();
	switch (instr.Itype.funct3) {
	case 0x0: // ADDI
		get(rs1);
		if (imm != 0) { c.i64_const(imm); c.op(I64_ADD); }
		set(rd);
		return true;
	case 0x2: // SLTI
		get(rs1); c.i64_const(imm); c.op(I64_LT_S); c.op(I64_EXTEND_I32_U); set(rd);
		return true;
	case 0x3: // SLTIU
		get(rs1); c.i64_const(imm); c.op(I64_LT_U); c.op(I64_EXTEND_I32_U); set(rd);
		return true;
	case 0x4: get(rs1); c.i64_const(imm); c.op(I64_XOR); set(rd); return true;
	case 0x6: get(rs1); c.i64_const(imm); c.op(I64_OR);  set(rd); return true;
	case 0x7: get(rs1); c.i64_const(imm); c.op(I64_AND); set(rd); return true;
	case 0x1:
		switch (instr.Itype.imm) {
		case 0x600: get(rs1); c.op(I64_CLZ); set(rd); return true;    // CLZ
		case 0x601: get(rs1); c.op(I64_CTZ); set(rd); return true;    // CTZ
		case 0x602: get(rs1); c.op(I64_POPCNT); set(rd); return true; // CPOP
		case 0x604: get(rs1); c.op(I64_EXTEND8_S); set(rd); return true;  // SEXT.B
		case 0x605: get(rs1); c.op(I64_EXTEND16_S); set(rd); return true; // SEXT.H
		}
		switch (instr.Itype.high_bits()) {
		case 0x000: get(rs1); c.i64_const(shamt); c.op(I64_SHL); set(rd); return true; // SLLI
		case 0x280: get(rs1); c.i64_const(int64_t(1) << shamt); c.op(I64_OR);  set(rd); return true; // BSETI
		case 0x480: get(rs1); c.i64_const(~(int64_t(1) << shamt)); c.op(I64_AND); set(rd); return true; // BCLRI
		case 0x680: get(rs1); c.i64_const(int64_t(1) << shamt); c.op(I64_XOR); set(rd); return true; // BINVI
		}
		return false;
	case 0x5:
		switch (instr.Itype.high_bits()) {
		case 0x000: get(rs1); c.i64_const(shamt); c.op(I64_SHR_U); set(rd); return true; // SRLI
		case 0x400: get(rs1); c.i64_const(shamt); c.op(I64_SHR_S); set(rd); return true; // SRAI
		case 0x600: get(rs1); c.i64_const(shamt); c.op(I64_ROTR);  set(rd); return true; // RORI
		case 0x480: // BEXTI
			get(rs1); c.i64_const(shamt); c.op(I64_SHR_U); c.i64_const(1); c.op(I64_AND); set(rd);
			return true;
		}
		return false;
	}
	return false;
}

// The upper 64 bits of the 128-bit product, built from 32-bit halves
// (wasm has no 64x64->128 multiply). MULHU, then the signed corrections:
// MULH subtracts (rs1 < 0 ? rs2 : 0) + (rs2 < 0 ? rs1 : 0), MULHSU only the first.
template <int W>
void WasmEmitter<W>::emit_mulh(unsigned rd, unsigned rs1, unsigned rs2, unsigned funct3)
{
	// T0 = a, T1 = b
	get(rs1); c.local_set(L_T0);
	get(rs2); c.local_set(L_T1);
	// T2 = p00 = a0*b0, T3 = p01 = a0*b1
	// middle = p10 + (p00 >> 32) + (uint32)p01
	// hi = p11 + (middle >> 32) + (p01 >> 32)
	auto lo = [&] (uint32_t l) { c.local_get(l); c.i64_const(0xFFFFFFFF); c.op(I64_AND); };
	auto hi = [&] (uint32_t l) { c.local_get(l); c.i64_const(32); c.op(I64_SHR_U); };
	lo(L_T0); lo(L_T1); c.op(I64_MUL); c.local_set(L_T2);   // p00
	lo(L_T0); hi(L_T1); c.op(I64_MUL); c.local_set(L_T3);   // p01
	// middle = a1*b0 + (p00 >> 32) + (p01 & 0xFFFFFFFF), kept in T2
	hi(L_T0); lo(L_T1); c.op(I64_MUL);
	hi(L_T2); c.op(I64_ADD);
	lo(L_T3); c.op(I64_ADD);
	c.local_set(L_T2);
	// hi = a1*b1 + (middle >> 32) + (p01 >> 32)
	hi(L_T0); hi(L_T1); c.op(I64_MUL);
	hi(L_T2); c.op(I64_ADD);
	hi(L_T3); c.op(I64_ADD);
	if (funct3 == 1 || funct3 == 2) {
		// - (a < 0 ? b : 0)
		c.local_get(L_T1); c.i64_const(0);
		c.local_get(L_T0); c.i64_const(0); c.op(I64_LT_S);
		c.select(); c.op(I64_SUB);
	}
	if (funct3 == 1) {
		// - (b < 0 ? a : 0)
		c.local_get(L_T0); c.i64_const(0);
		c.local_get(L_T1); c.i64_const(0); c.op(I64_LT_S);
		c.select(); c.op(I64_SUB);
	}
	set(rd);
}

// Handle RISC-V division by zero and signed overflow before emitting Wasm
// division. Wasm rem_s already returns zero for INT_MIN % -1.
template <int W>
void WasmEmitter<W>::emit_divrem(unsigned rd, unsigned rs1, unsigned rs2, unsigned funct3, bool word)
{
	const uint8_t T = word ? I32 : I64;
	auto a = [&] { get(rs1); if (word) c.op(I32_WRAP_I64); };
	auto b = [&] { get(rs2); if (word) c.op(I32_WRAP_I64); };
	auto eqz = [&] { c.op(word ? I32_EQZ : I64_EQZ); };
	auto const_ = [&] (int64_t v) { if (word) c.i32_const(int32_t(v)); else c.i64_const(v); };

	b(); eqz();
	if_(T);
		if (funct3 == 4 || funct3 == 5) const_(-1); // DIV(U): all ones
		else a();                                  // REM(U): the dividend
	else_();
		switch (funct3) {
		case 4: // DIV: INT_MIN / -1 = INT_MIN
			a(); const_(word ? INT32_MIN : INT64_MIN); c.op(word ? I32_EQ : I64_EQ);
			b(); const_(-1); c.op(word ? I32_EQ : I64_EQ);
			c.op(I32_AND);
			if_(T);
				a();
			else_();
				a(); b(); c.op(word ? I32_DIV_S : I64_DIV_S);
			end_if();
			break;
		case 5: a(); b(); c.op(word ? I32_DIV_U : I64_DIV_U); break;
		case 6: a(); b(); c.op(word ? I32_REM_S : I64_REM_S); break;
		case 7: a(); b(); c.op(word ? I32_REM_U : I64_REM_U); break;
		}
	end_if();
	if (word) c.op(I64_EXTEND_I32_S);
	set(rd);
}

template <int W>
bool WasmEmitter<W>::emit_op(const rv32i_instruction instr)
{
	const unsigned rd = instr.Rtype.rd, rs1 = instr.Rtype.rs1, rs2 = instr.Rtype.rs2;
	const unsigned f3 = instr.Rtype.funct3;
	switch (instr.Rtype.funct7) {
	case 0x00: {
		switch (f3) {
		case 0: bin64(rd, rs1, rs2, I64_ADD); return true;
		case 1: bin64(rd, rs1, rs2, I64_SHL); return true;   // Masked by 63, like RISC-V
		case 2: compare64(rd, rs1, rs2, I64_LT_S); return true;
		case 3: compare64(rd, rs1, rs2, I64_LT_U); return true;
		case 4: bin64(rd, rs1, rs2, I64_XOR); return true;
		case 5: bin64(rd, rs1, rs2, I64_SHR_U); return true;
		case 6: bin64(rd, rs1, rs2, I64_OR); return true;
		case 7: bin64(rd, rs1, rs2, I64_AND); return true;
		}
		return false;
	}
	case 0x20:
		switch (f3) {
		case 0: bin64(rd, rs1, rs2, I64_SUB); return true;
		case 5: bin64(rd, rs1, rs2, I64_SHR_S); return true;
		case 4: get(rs1); get(rs2); c.i64_const(-1); c.op(I64_XOR); c.op(I64_XOR); set(rd); return true; // XNOR
		case 6: get(rs1); get(rs2); c.i64_const(-1); c.op(I64_XOR); c.op(I64_OR);  set(rd); return true; // ORN
		case 7: get(rs1); get(rs2); c.i64_const(-1); c.op(I64_XOR); c.op(I64_AND); set(rd); return true; // ANDN
		}
		return false;
	case 0x01: // M
		switch (f3) {
		case 0: bin64(rd, rs1, rs2, I64_MUL); return true;
		case 1: case 2: case 3: emit_mulh(rd, rs1, rs2, f3); return true;
		default: emit_divrem(rd, rs1, rs2, f3, false); return true;
		}
	case 0x10: // Zba: SH1ADD, SH2ADD, SH3ADD
		if (f3 == 2 || f3 == 4 || f3 == 6) {
			get(rs1); c.i64_const(f3 >> 1); c.op(I64_SHL); get(rs2); c.op(I64_ADD); set(rd);
			return true;
		}
		return false;
	case 0x05: // Zbb: MIN, MINU, MAX, MAXU
		switch (f3) {
		case 4: select_minmax(rd, rs1, rs2, I64_LT_S); return true;
		case 5: select_minmax(rd, rs1, rs2, I64_LT_U); return true;
		case 6: select_minmax(rd, rs1, rs2, I64_GT_S); return true;
		case 7: select_minmax(rd, rs1, rs2, I64_GT_U); return true;
		}
		return false;
	case 0x30: // Zbb: ROL, ROR
		if (f3 == 1) { bin64(rd, rs1, rs2, I64_ROTL); return true; }
		if (f3 == 5) { bin64(rd, rs1, rs2, I64_ROTR); return true; }
		return false;
	case 0x14: // Zbs: BSET
		if (f3 == 1) { get(rs1); c.i64_const(1); get(rs2); c.op(I64_SHL); c.op(I64_OR); set(rd); return true; }
		return false;
	case 0x24: // Zbs: BCLR, BEXT
		if (f3 == 1) {
			get(rs1); c.i64_const(1); get(rs2); c.op(I64_SHL); c.i64_const(-1); c.op(I64_XOR);
			c.op(I64_AND); set(rd);
			return true;
		}
		if (f3 == 5) { get(rs1); get(rs2); c.op(I64_SHR_U); c.i64_const(1); c.op(I64_AND); set(rd); return true; }
		return false;
	case 0x34: // Zbs: BINV
		if (f3 == 1) { get(rs1); c.i64_const(1); get(rs2); c.op(I64_SHL); c.op(I64_XOR); set(rd); return true; }
		return false;
	case 0x07: // Zicond: CZERO.EQZ, CZERO.NEZ
		if (f3 == 5 || f3 == 7) {
			// select(a, b, cond) is cond ? a : b. EQZ keeps rs1 when rs2 != 0,
			// NEZ keeps it when rs2 == 0.
			get(rs1); c.i64_const(0); get(rs2); c.op(I64_EQZ);
			if (f3 == 5) c.op(I32_EQZ);
			c.select();
			set(rd);
			return true;
		}
		return false;
	}
	return false;
}

template <int W>
bool WasmEmitter<W>::emit_op_imm32(const rv32i_instruction instr)
{
	const unsigned rd = instr.Itype.rd, rs1 = instr.Itype.rs1;
	const unsigned imm = instr.Itype.imm;
	switch (instr.Itype.funct3) {
	case 0x0: // ADDIW
		get(rs1);
		if (instr.Itype.signed_imm() != 0) { c.i64_const(instr.Itype.signed_imm()); c.op(I64_ADD); }
		c.op(I64_EXTEND32_S); set(rd);
		return true;
	case 0x1:
		if ((imm & 0xFE0) == 0) { // SLLIW
			get(rs1); c.op(I32_WRAP_I64); c.i32_const(imm & 31); c.op(I32_SHL); c.op(I64_EXTEND_I32_S); set(rd);
			return true;
		}
		if ((imm >> 6) == 0x02) { // SLLI.UW
			zext32(rs1); c.i64_const(imm & 63); c.op(I64_SHL); set(rd);
			return true;
		}
		switch (imm) {
		case 0x600: get(rs1); c.op(I32_WRAP_I64); c.op(I32_CLZ); c.op(I64_EXTEND_I32_U); set(rd); return true; // CLZW
		case 0x601: get(rs1); c.op(I32_WRAP_I64); c.op(I32_CTZ); c.op(I64_EXTEND_I32_U); set(rd); return true; // CTZW
		case 0x602: get(rs1); c.op(I32_WRAP_I64); c.op(I32_POPCNT); c.op(I64_EXTEND_I32_U); set(rd); return true; // CPOPW
		}
		return false;
	case 0x5: {
		uint8_t op = 0;
		switch (imm & 0xFE0) {
		case 0x000: op = I32_SHR_U; break; // SRLIW
		case 0x400: op = I32_SHR_S; break; // SRAIW
		case 0x600: op = I32_ROTR;  break; // RORIW
		default: return false;
		}
		get(rs1); c.op(I32_WRAP_I64); c.i32_const(imm & 31); c.op(op); c.op(I64_EXTEND_I32_S); set(rd);
		return true;
	}
	}
	return false;
}

template <int W>
bool WasmEmitter<W>::emit_op32(const rv32i_instruction instr)
{
	const unsigned rd = instr.Rtype.rd, rs1 = instr.Rtype.rs1, rs2 = instr.Rtype.rs2;
	const unsigned f3 = instr.Rtype.funct3;
	switch (instr.Rtype.funct7) {
	case 0x00:
		switch (f3) {
		case 0: bin32(rd, rs1, rs2, I32_ADD); return true;
		case 1: bin32(rd, rs1, rs2, I32_SHL); return true;
		case 5: bin32(rd, rs1, rs2, I32_SHR_U); return true;
		}
		return false;
	case 0x20:
		switch (f3) {
		case 0: bin32(rd, rs1, rs2, I32_SUB); return true;
		case 5: bin32(rd, rs1, rs2, I32_SHR_S); return true;
		}
		return false;
	case 0x01:
		if (f3 == 0) { bin32(rd, rs1, rs2, I32_MUL); return true; }
		if (f3 >= 4) { emit_divrem(rd, rs1, rs2, f3, true); return true; }
		return false;
	case 0x04:
		if (f3 == 0) { zext32(rs1); get(rs2); c.op(I64_ADD); set(rd); return true; } // ADD.UW
		if (f3 == 4 && rs2 == 0) { get(rs1); c.i64_const(0xFFFF); c.op(I64_AND); set(rd); return true; } // ZEXT.H
		return false;
	case 0x10: // SH1ADD.UW, SH2ADD.UW, SH3ADD.UW
		if (f3 == 2 || f3 == 4 || f3 == 6) {
			zext32(rs1); c.i64_const(f3 >> 1); c.op(I64_SHL); get(rs2); c.op(I64_ADD); set(rd);
			return true;
		}
		return false;
	case 0x30:
		if (f3 == 1) { bin32(rd, rs1, rs2, I32_ROTL); return true; }
		if (f3 == 5) { bin32(rd, rs1, rs2, I32_ROTR); return true; }
		return false;
	}
	return false;
}

struct WasmModuleOutput
{
	std::vector<uint8_t> bytes;
	std::vector<std::pair<uint64_t, unsigned>> mappings; // entry address -> function
	unsigned functions = 0;
	size_t instructions = 0;
	size_t fallbacks = 0;
};

static void wasm_functype(wasm::Buffer& b, std::initializer_list<uint8_t> params, std::initializer_list<uint8_t> results)
{
	b.u8(0x60);
	b.u32(params.size());  for (auto p : params) b.u8(p);
	b.u32(results.size()); for (auto r : results) b.u8(r);
}

static void wasm_import_func(wasm::Buffer& b, std::string_view mod, std::string_view name, uint32_t type)
{
	b.name(mod); b.name(name); b.u8(0x00); b.u32(type);
}

template <int W>
static WasmModuleOutput wasm_emit_module(const MachineOptions<W>& options, TransBlocks<W>& discovered,
	const WasmLayout& layout, uint32_t hash)
{
	WasmModuleOutput result;
	std::vector<Code> bodies;
	bodies.reserve(discovered.blocks.size());

	for (auto& block : discovered.blocks) {
		block.blocks = &discovered.blocks;
		WasmEmitter<W> e(block, layout, options);
		auto func = e.emit();
		const unsigned index = bodies.size();
		for (const auto addr : func.entries)
			result.mappings.push_back({ addr, index });
		result.instructions += block.instr.size();
		result.fallbacks += func.fallbacks;
		bodies.push_back(std::move(func.code));
	}
	result.functions = bodies.size();

	wasm::Buffer m;
	static constexpr uint8_t magic[8] = { 0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00 };
	m.bytes(magic, sizeof(magic));

	{ // Types
		wasm::Buffer s;
		s.u32(TYPE_COUNT);
		wasm_functype(s, { I32, I32, I64, I64, I64 }, {});      // TYPE_BLOCK
		wasm_functype(s, { I32, I64, I32, I64 }, { I64 });      // TYPE_LOAD
		wasm_functype(s, { I32, I64, I64, I32, I64 }, {});      // TYPE_STORE
		wasm_functype(s, { I32, I64, I64, I64, I64 }, { I32 }); // TYPE_SYSCALL
		wasm_functype(s, { I32, I32, I32, I64, I64, I64 }, { I32 }); // TYPE_EXECUTE
		wasm_functype(s, { I32, I32, I32, I64 }, {});           // TYPE_EXECUTE_SIMPLE
		wasm_functype(s, { F32, F32, F32 }, { F32 });           // TYPE_FMA32
		wasm_functype(s, { F64, F64, F64 }, { F64 });           // TYPE_FMA64
		m.section(SEC_TYPE, s);
	}
	{ // Omit the table import so the shared table does not retain this instance.
		wasm::Buffer s;
		s.u32(IMP_COUNT + 1);
		wasm_import_func(s, "api", "load", TYPE_LOAD);
		wasm_import_func(s, "api", "store", TYPE_STORE);
		wasm_import_func(s, "api", "syscall", TYPE_SYSCALL);
		wasm_import_func(s, "api", "execute", TYPE_EXECUTE);
		wasm_import_func(s, "api", "execute_simple", TYPE_EXECUTE_SIMPLE);
		wasm_import_func(s, "api", "fma32", TYPE_FMA32);
		wasm_import_func(s, "api", "fma64", TYPE_FMA64);
		s.name("env"); s.name("memory"); s.u8(0x02); s.u8(0x00); s.u32(0); // min 0, no max
		m.section(SEC_IMPORT, s);
	}
	{ // Functions
		wasm::Buffer s;
		s.u32(result.functions);
		for (unsigned i = 0; i < result.functions; i++) s.u32(TYPE_BLOCK);
		m.section(SEC_FUNCTION, s);
	}
	{ // Exports: function i as "i", placed in the function table by the linker
		wasm::Buffer s;
		s.u32(result.functions);
		for (unsigned i = 0; i < result.functions; i++) {
			s.name(std::to_string(i)); s.u8(0x00); s.u32(IMP_COUNT + i);
		}
		m.section(SEC_EXPORT, s);
	}
	{ // Code
		wasm::Buffer s;
		s.u32(result.functions);
		for (auto& body : bodies) {
			s.u32(body.size());
			s.append(body);
		}
		m.section(SEC_CODE, s);
	}
	{ // Translation hash
		wasm::Buffer s;
		s.name("libriscv.hash");
		s.bytes(&hash, sizeof(hash));
		m.section(SEC_CUSTOM, s);
	}
	result.bytes = std::move(m.data);
	return result;
}

// Runtime imports

template <int W>
static uint64_t rvw_load(CPU<W>* cpu, uint64_t addr, uint32_t funct3, uint64_t pc)
{
	cpu->registers().pc = pc;
	auto& mem = cpu->machine().memory;
	switch (funct3) {
	case 0: return int64_t(mem.template read<int8_t>(addr));
	case 1: return int64_t(mem.template read<int16_t>(addr));
	case 2: return int64_t(mem.template read<int32_t>(addr));
	case 3: return mem.template read<uint64_t>(addr);
	case 4: return mem.template read<uint8_t>(addr);
	case 5: return mem.template read<uint16_t>(addr);
	case 6: return mem.template read<uint32_t>(addr);
	}
	CPU<W>::trigger_exception(ILLEGAL_OPERATION, addr);
}
template <int W>
static void rvw_store(CPU<W>* cpu, uint64_t addr, uint64_t value, uint32_t funct3, uint64_t pc)
{
	cpu->registers().pc = pc;
	auto& mem = cpu->machine().memory;
	switch (funct3) {
	case 0: mem.template write<uint8_t>(addr, value); return;
	case 1: mem.template write<uint16_t>(addr, value); return;
	case 2: mem.template write<uint32_t>(addr, value); return;
	case 3: mem.template write<uint64_t>(addr, value); return;
	}
	CPU<W>::trigger_exception(ILLEGAL_OPERATION, addr);
}
// Return 1 to resume dispatch at the updated PC.
template <int W>
static uint32_t rvw_syscall(CPU<W>* cpu, uint64_t pc, uint64_t ic, uint64_t max, uint64_t sysno)
{
	auto& machine = cpu->machine();
	cpu->registers().pc = pc;
	machine.set_instruction_counter(ic);
	machine.set_max_instructions(max);
	machine.system_call(sysno);
	// Match the syscall PC adjustment in cpu_dispatch.cpp.
	if (cpu->registers().pc != pc || machine.instruction_counter() >= machine.max_instructions()) {
		cpu->registers().pc += 4;
		return 1;
	}
	return 0;
}
template <int W>
static uint32_t rvw_execute(CPU<W>* cpu, uint32_t instr, uint32_t handler, uint64_t pc, uint64_t ic, uint64_t max)
{
	auto& machine = cpu->machine();
	cpu->registers().pc = pc;
	machine.set_instruction_counter(ic);
	machine.set_max_instructions(max);
	DecoderData<W>::get_handlers()[handler](*cpu, rv32i_instruction{instr});
	if (machine.instruction_counter() >= machine.max_instructions()) {
		cpu->registers().pc = pc + rv32i_instruction{instr}.length();
		return 1;
	}
	return 0;
}

template <int W>
static void rvw_execute_simple(CPU<W>* cpu, uint32_t instr, uint32_t handler, uint64_t pc)
{
	cpu->registers().pc = pc;
	DecoderData<W>::get_handlers()[handler](*cpu, rv32i_instruction{instr});
}

[[maybe_unused]] static float  rvw_fma32(float a, float b, float c) { return std::fma(a, b, c); }
[[maybe_unused]] static double rvw_fma64(double a, double b, double c) { return std::fma(a, b, c); }

#ifdef __EMSCRIPTEN__
// Keep wasmTable available to EM_JS with -fwasm-exceptions.
EM_JS_DEPS(rvw_translation_deps, "$wasmTable");
// Return the first table slot, -1 on failure, or -2 for async compilation.
// Async results are delivered to rvw_async_linked. A browser can reject large
// synchronous compilations with RangeError.
EM_JS(int, rvw_link_module, (const uint8_t* bytes, uint32_t len, uint32_t nfuncs,
	const uint32_t* imports, uint32_t nimports, int verbose, uint32_t token, int force_async), {
	const names = ['load', 'store', 'syscall', 'execute', 'execute_simple', 'fma32', 'fma64'];
	const api = {};
	for (let i = 0; i < nimports; i++)
		api[names[i]] = wasmTable.get(HEAPU32[(imports >> 2) + i]);
	const link = (module) => {
		const exports = new WebAssembly.Instance(module, {
			env: { memory: wasmMemory }, api: api
		}).exports;
		// Table slots released by earlier translations are reused (first fit)
		if (!Module.rvwFreeSlots) Module.rvwFreeSlots = [];
		const free = Module.rvwFreeSlots;
		let base = -1;
		for (let i = 0; i < free.length; i++) {
			if (free[i].count >= nfuncs) {
				base = free[i].base;
				free[i].base += nfuncs; free[i].count -= nfuncs;
				if (free[i].count == 0) free.splice(i, 1);
				break;
			}
		}
		if (base < 0) base = wasmTable.grow(nfuncs);
		for (let i = 0; i < nfuncs; i++) {
			wasmTable.set(base + i, exports[i]);
			if (typeof wasmTableMirror !== 'undefined') wasmTableMirror[base + i] = undefined;
		}
		return base;
	};
	const t0 = performance.now();
	if (!force_async) {
		try {
			const module = new WebAssembly.Module(HEAPU8.subarray(bytes, bytes + len));
			const t1 = performance.now();
			const base = link(module);
			if (verbose)
				out('libriscv: WebAssembly module compiled in ' + (t1 - t0).toFixed(2) +
					' ms, instantiated in ' + (performance.now() - t1).toFixed(2) + ' ms');
			return base;
		} catch (e) {
			if (!(e instanceof RangeError)) {
				err('libriscv: WebAssembly translation failed to link: ' + e);
				return -1;
			}
		}
	}
	// Copy the bytes before async compilation outlives the caller buffer.
	WebAssembly.compile(HEAPU8.slice(bytes, bytes + len)).then((module) => {
		let base = -1;
		try {
			base = link(module);
			if (verbose)
				out('libriscv: WebAssembly module compiled asynchronously in ' +
					(performance.now() - t0).toFixed(2) + ' ms');
		} catch (e) {
			err('libriscv: WebAssembly translation failed to link: ' + e);
		}
		_rvw_async_linked(token, base);
	}, (e) => {
		err('libriscv: WebAssembly translation failed to compile: ' + e);
		_rvw_async_linked(token, -1);
	});
	return -2;
});
// Clears the slots of a released translation, so that a stale index traps
EM_JS(void, rvw_unlink_module, (uint32_t base, uint32_t count), {
	for (let i = 0; i < count; i++) {
		wasmTable.set(base + i, null);
		if (typeof wasmTableMirror !== 'undefined') wasmTableMirror[base + i] = undefined;
	}
	if (!Module.rvwFreeSlots) Module.rvwFreeSlots = [];
	Module.rvwFreeSlots.push({ base: base, count: count });
});

static std::unordered_map<uint32_t, std::function<void(int)>> g_pending_links;
static uint32_t g_next_link_token = 1;

extern "C" EMSCRIPTEN_KEEPALIVE void rvw_async_linked(uint32_t token, int base)
{
	auto it = g_pending_links.find(token);
	if (it == g_pending_links.end())
		return;
	auto complete = std::move(it->second);
	g_pending_links.erase(it);
	try {
		complete(base);
	} catch (const std::exception& e) {
		fprintf(stderr, "libriscv: WebAssembly translation failed to activate: %s\n", e.what());
	}
}
#endif

template <int W>
void CPU<W>::wasm_translate(const MachineOptions<W>& options,
	std::shared_ptr<DecodedExecuteSegment<W>>& shared_segment, bool live_patch) const
{
	if constexpr (W != 8) {
		if (options.verbose_loader)
			printf("libriscv: The WebAssembly translation backend is RV64 only\n");
		return;
	} else {
	auto& exec = *shared_segment;
	using clock = std::chrono::steady_clock;
	const auto t0 = clock::now();

	TransBlocks<W> discovered;
	timespec ts {};
	this->discover_translation_blocks(options, exec, discovered, false, ts);
	if (discovered.blocks.empty())
		return;

	auto& m = const_cast<Machine<W>&>(machine());
	const uintptr_t cpu_addr = uintptr_t(this);
	WasmLayout layout;
	layout.regs_off  = uintptr_t(&this->registers().get()[0]) - cpu_addr;
	layout.pc_off    = uintptr_t(&this->registers().pc) - cpu_addr;
	layout.fregs_off = uintptr_t(&this->registers().getfl(0)) - cpu_addr;
	layout.arena_off = uintptr_t(&m.memory.memory_arena_ptr_ref()) - cpu_addr;
	layout.counter_off = uintptr_t(&m.get_counters().first) - cpu_addr;
	if (uintptr_t(&m.get_counters().second) != uintptr_t(&m.get_counters().first) + 8)
		throw MachineException(INVALID_PROGRAM, "wasm translator: unexpected counter layout");

	auto module = wasm_emit_module<W>(options, discovered, layout, exec.translation_hash());
	const auto t1 = clock::now();

	if (const char* path = getenv("RVWASM_DUMP"); path != nullptr) {
		std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
		ofs.write((const char*)module.bytes.data(), module.bytes.size());
	}
	if (options.verbose_loader || options.translate_timing) {
		printf("libriscv: WebAssembly translation: %u functions, %zu instructions (%zu by handler), "
			"%zu mappings, %zu bytes, generated in %.2f ms\n",
			module.functions, module.instructions, module.fallbacks, module.mappings.size(),
			module.bytes.size(), std::chrono::duration<double, std::milli>(t1 - t0).count());
	}

#ifdef __EMSCRIPTEN__
	const unsigned nfuncs = module.functions;
	std::vector<std::pair<address_t, unsigned>> addr_mappings;
	addr_mappings.reserve(module.mappings.size());
	for (const auto& [addr, index] : module.mappings)
		addr_mappings.push_back({ address_t(addr), index });

	auto activate = [nfuncs, t0] (const MachineOptions<W>& options, DecodedExecuteSegment<W>& exec, int base,
		const std::vector<std::pair<address_t, unsigned>>& addr_mappings, bool live_patch)
	{
		exec.set_translation_owner(std::shared_ptr<void>(nullptr, [base, nfuncs] (void*) {
			rvw_unlink_module(base, nfuncs);
		}));
		// A function pointer is a table index in wasm32
		auto& mappings = exec.create_mappings(nfuncs + 1);
		for (unsigned i = 0; i < nfuncs; i++)
			mappings[i] = reinterpret_cast<bintr_block_func<W>>(uintptr_t(base + i));
		exec.set_mapping(nfuncs, [] (CPU<W>&, uint64_t, uint64_t, address_t) -> bintr_block_returns<W> {
			throw MachineException(INVALID_PROGRAM, "Translation mapping outside execute area");
		});
		CPU<W>::apply_translator_mappings(options, exec, addr_mappings, nfuncs, live_patch);
		if (options.verbose_loader || options.translate_timing) {
			printf("libriscv: Activated WebAssembly translation with hash 0x%X after %.2f ms%s\n",
				exec.translation_hash(),
				std::chrono::duration<double, std::milli>(clock::now() - t0).count(),
				live_patch ? ", live-patching enabled" : "");
		}
	};

	const uint32_t imports[IMP_COUNT] = {
		uint32_t(uintptr_t(&rvw_load<W>)),
		uint32_t(uintptr_t(&rvw_store<W>)),
		uint32_t(uintptr_t(&rvw_syscall<W>)),
		uint32_t(uintptr_t(&rvw_execute<W>)),
		uint32_t(uintptr_t(&rvw_execute_simple<W>)),
		uint32_t(uintptr_t(&rvw_fma32)),
		uint32_t(uintptr_t(&rvw_fma64)),
	};
	const char* force_async = getenv("LIBRISCV_WASM_ASYNC");
	const uint32_t token = g_next_link_token++;
	const int base = rvw_link_module(module.bytes.data(), module.bytes.size(), nfuncs,
		imports, IMP_COUNT, options.verbose_loader, token,
		force_async != nullptr && force_async[0] == '1');

	if (base >= 0) {
		activate(options, exec, base, addr_mappings, live_patch);
	} else if (base == -2) {
		// Async activation uses live patching because dispatch may already hold
		// the segment's decoder cache.
		std::weak_ptr<DecodedExecuteSegment<W>> weak = shared_segment;
		g_pending_links.emplace(token,
			[weak, options, addr_mappings = std::move(addr_mappings), activate, nfuncs] (int base) {
				if (base < 0) return;
				auto segment = weak.lock();
				if (!segment || segment->is_binary_translated()) {
					rvw_unlink_module(base, nfuncs);
					return;
				}
				activate(options, *segment, base, addr_mappings, true);
			});
		if (options.verbose_loader)
			printf("libriscv: WebAssembly translation is compiling asynchronously\n");
	}
#else
	(void)live_patch;
	if (options.verbose_loader)
		printf("libriscv: WebAssembly translations can only be linked in an Emscripten build\n");
#endif
	} // W == 8
}

#ifdef RISCV_32I
template void CPU<4>::wasm_translate(const MachineOptions<4>&, std::shared_ptr<DecodedExecuteSegment<4>>&, bool) const;
#endif
#ifdef RISCV_64I
template void CPU<8>::wasm_translate(const MachineOptions<8>&, std::shared_ptr<DecodedExecuteSegment<8>>&, bool) const;
#endif
#ifdef RISCV_128I
template void CPU<16>::wasm_translate(const MachineOptions<16>&, std::shared_ptr<DecodedExecuteSegment<16>>&, bool) const;
#endif
} // riscv
