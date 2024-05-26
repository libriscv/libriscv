#include "machine.hpp"
#include "instruction_list.hpp"
#include <inttypes.h>
#include "rv32i_instr.hpp"
#include "rvfd.hpp"
#include "tr_types.hpp"
#ifdef RISCV_EXT_VECTOR
#include "rvv.hpp"
#endif

#define PCRELA(x) ((address_t) (this->pc() + (x)))
#define PCRELS(x) std::to_string(PCRELA(x)) + "UL"
#define STRADDR(x) (std::to_string(x) + "UL")
#define UNKNOWN_INSTRUCTION() { code += "api.execute(cpu, " + std::to_string(instr.whole) + ");\n"; }

namespace riscv {
static const std::string LOOP_EXPRESSION = "counter < max_counter";
static const std::string SIGNEXTW = "(saddr_t) (int32_t)";
static constexpr int ALIGN_MASK = (compressed_enabled) ? 0x1 : 0x3;

template <int W>
static std::string funclabel(const std::string& func, uint64_t addr) {
	char buf[32];
	if (const int len = snprintf(buf, sizeof(buf), "%s_%" PRIx64, func.c_str(), addr); len > 0)
		return std::string(buf, len);
	throw MachineException(INVALID_PROGRAM, "Failed to format function label");
}
#define FUNCLABEL(addr) funclabel<W>(func, addr)

struct BranchInfo {
	bool sign;
	bool goto_enabled;
	uint64_t jump_pc;
	uint64_t call_pc;
};

template <int W>
struct Emitter
{
	static constexpr bool CACHED_REGISTERS = false;
	static constexpr unsigned XLEN = W * 8u;
	using address_t = address_type<W>;

	Emitter(CPU<W>& c, const TransInfo<W>& ptinfo)
		: cpu(c), m_pc(ptinfo.basepc), tinfo(ptinfo)
	{
		this->func = funclabel<W>("f", this->pc());
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
		if (UNLIKELY(reg == 0))
			throw MachineException(INVALID_PROGRAM, "Attempt to cache register x0");
		if (!gprs[reg]) {
			gprs[reg] = true;
			bool exists = gpr_exists[reg];
			if (exists == false) {
				gpr_exists[reg] = true;
			} else {
				add_code(loaded_regname(reg) + " = cpu->r[" + std::to_string(reg) + "];");
			}
		}
	}
	void invalidate_register(int reg) {
		if constexpr (CACHED_REGISTERS) {
			gpr_exists[reg] = true;
			gprs[reg] = false;
		}
	}
	void potentially_reload_register(int reg) {
		if constexpr (CACHED_REGISTERS) {
			if (gpr_exists[reg]) {
				add_code(loaded_regname(reg) + " = cpu->r[" + std::to_string(reg) + "];");
				gprs[reg] = true;
			}
		}
	}
	void potentially_reload_all_registers() {
		for (int reg = 1; reg < 32; reg++) {
			this->potentially_reload_register(reg);
		}
	}
	void realize_registers(int x0, int x1) {
		for (int reg = x0; reg < x1; reg++) {
			if (gprs[reg]) {
				add_code("cpu->r[" + std::to_string(reg) + "] = " + loaded_regname(reg) + ";");
			}
		}
	}
	void restore_syscall_registers() {
		if constexpr (CACHED_REGISTERS) {
			this->realize_registers(10, 18);
		}
	}
	void restore_all_registers() {
		if constexpr (CACHED_REGISTERS) {
			this->realize_registers(0, 32);
		}
	}
	void exit_function(const std::string& new_pc, bool add_bracket = false)
	{
		if constexpr (CACHED_REGISTERS) {
			this->restore_all_registers();
		}
		add_code(
			"cpu->pc = " + new_pc + ";",
			"return (ReturnValues){counter, max_counter};", (add_bracket) ? " }" : "");
	}

	std::string from_reg(int reg) {
		if (reg == 3 && tinfo.gp != 0)
			return std::to_string(tinfo.gp);
		else if (reg != 0) {
			if constexpr (CACHED_REGISTERS) {
				load_register(reg);
				return loaded_regname(reg);
			} else {
				return "cpu->r[" + std::to_string(reg) + "]";
			}
		}
		return "(addr_t)0";
	}
	std::string to_reg(int reg) {
		if (reg != 0) {
			if constexpr (CACHED_REGISTERS) {
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

	void add_branch(const BranchInfo& binfo, const std::string& op);

	bool gpr_exists_at(int reg) const noexcept { return this->gpr_exists.at(reg); }
	auto& get_gpr_exists() const noexcept { return this->gpr_exists; }

	template <typename T>
	void memory_load(std::string dst, std::string type, int reg, int32_t imm)
	{
		const std::string data = "rpage" + PCRELS(0);
		std::string cast;
		if constexpr (std::is_signed_v<T>) {
			cast = "(saddr_t)";
		}

		if (reg == REG_GP && tinfo.gp != 0x0 && cpu.machine().memory.uses_flat_memory_arena())
		{
			/* XXX: Check page permissions here? */
			const address_t absolute_vaddr = tinfo.gp + imm;
			if (absolute_vaddr >= 0x1000 && absolute_vaddr + sizeof(T) <= this->cpu.machine().memory.memory_arena_size()) {
				add_code(
					dst + " = " + cast + "*(" + type + "*)&arena_base[" + speculation_safe(absolute_vaddr) + "];"
				);
				return;
			}
		}

		const auto address = from_reg(reg) + " + " + from_imm(imm);
		if (cpu.machine().memory.uses_flat_memory_arena()) {
			add_code(
				"if (LIKELY(ARENA_READABLE(" + address + ")))",
					dst + " = " + cast + "*(" + type + "*)&arena_base[" + speculation_safe(address) + "];",
				"else {",
					"const char* " + data + " = api.mem_ld(cpu, PAGENO(" + address + "));",
					dst + " = " + cast + "*(" + type + "*)&" + data + "[PAGEOFF(" + address + ")];",
				"}");
		} else {
			add_code(
				"const char* " + data + " = api.mem_ld(cpu, PAGENO(" + address + "));",
				dst + " = " + cast + "*(" + type + "*)&" + data + "[PAGEOFF(" + address + ")];"
			);
		}
	}
	void memory_store(std::string type, int reg, int32_t imm, std::string value)
	{
		const std::string data = "wpage" + PCRELS(0);

		if (reg == REG_GP && tinfo.gp != 0x0 && cpu.machine().memory.uses_flat_memory_arena())
		{
			/* XXX: Check page permissions */
			const address_t absolute_vaddr = tinfo.gp + imm;
			if (absolute_vaddr >= this->cpu.machine().memory.initial_rodata_end() && absolute_vaddr < this->cpu.machine().memory.memory_arena_size()) {
				add_code("*(" + type + "*)&arena_base[" + speculation_safe(absolute_vaddr) + "] = " + value + ";");
			}
			return;
		}

		const auto address = from_reg(reg) + " + " + from_imm(imm);
		if (cpu.machine().memory.uses_flat_memory_arena()) {
			add_code(
				"if (LIKELY(ARENA_WRITABLE(" + address + ")))",
				"  *(" + type + "*)&arena_base[" + speculation_safe(address) + "] = " + value + ";",
				"else {",
				"  char *" + data + " = api.mem_st(cpu, PAGENO(" + address + "));",
				"  *(" + type + "*)&" + data + "[PAGEOFF(" + address + ")] = " + value + ";",
				"}");
		} else {
			add_code("char* " + data + " = api.mem_st(cpu, PAGENO(" + address + "));");
			add_code(
				"*(" + type + "*)&" + data + "[PAGEOFF(" + address + ")] = " + value + ";"
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

	void add_reentry_next() {
		// Avoid re-entering at the end of the function
		// WARNING: End-of-function can be empty
		if (this->pc() + this->instr.length() >= end_pc())
			return;
		this->mapping_labels.insert(index() + 1);
		//code.append(FUNCLABEL(this->pc() + 4) + ":;\n");
	}

	uint64_t reset_and_get_icounter() {
		auto result = this->m_instr_counter;
		this->m_instr_counter = 0;
		return result;
	}
	void increment_counter_so_far() {
		auto icount = this->reset_and_get_icounter();
		if (icount > 0)
			code.append("counter += " + std::to_string(icount) + ";\n");
	}

	bool block_exists(address_t pc) const noexcept {
		for (auto& blk : *tinfo.blocks) {
			if (blk.basepc == pc) return true;
		}
		return false;
	}

	size_t index() const noexcept { return this->m_idx; }
	address_t pc() const noexcept { return this->m_pc; }
	address_t begin_pc() const noexcept { return tinfo.basepc; }
	address_t end_pc() const noexcept { return tinfo.endpc; }
	const std::string get_func() const noexcept { return this->func; }
	void emit();

private:
	static std::string speculation_safe(const std::string& address) {
		return "SPECSAFE(" + address + ")";
	}
	static std::string speculation_safe(const address_t address) {
		return "SPECSAFE(" + std::to_string(address) + ")";
	}

	std::string code;
	CPU<W>& cpu;
	size_t m_idx = 0;
	address_t m_pc = 0x0;
	rv32i_instruction instr;
	uint64_t m_instr_counter = 0;

	std::array<bool, 32> gprs {};
	std::array<bool, 32> gpr_exists {};

	std::string func;
	const TransInfo<W>& tinfo;

	std::vector<TransMapping<W>> mappings;
	std::set<unsigned> labels;
	std::set<unsigned> mapping_labels;
	std::set<address_t> pagedata;
};

template <int W>
inline void Emitter<W>::add_branch(const BranchInfo& binfo, const std::string& op)
{
	using address_t = address_type<W>;
	if (binfo.sign == false)
		code += "if (" + from_reg(instr.Btype.rs1) + op + from_reg(instr.Btype.rs2) + ") {\n";
	else
		code += "if ((saddr_t)" + from_reg(instr.Btype.rs1) + op + " (saddr_t)" + from_reg(instr.Btype.rs2) + ") {\n";

	if (UNLIKELY(PCRELA(instr.Btype.signed_imm()) & ALIGN_MASK))
	{
		// TODO: Make exception a helper function, as return values are implementation detail
		code +=
			"api.exception(cpu, MISALIGNED_INSTRUCTION); return (ReturnValues){0, 0};\n"
			"}\n";
		return;
	}

	if (binfo.goto_enabled) {
		// this is a jump back to the start of the function
		code += "if (" + LOOP_EXPRESSION + ") goto " + func + "_start;\n";
	} else if (binfo.jump_pc != 0) {
		if (binfo.jump_pc > this->pc()) {
			// unconditional forward jump + bracket
			code += "goto " + FUNCLABEL(binfo.jump_pc) + "; }\n";
			return;
		}
		// backward jump
		code += "if (" + LOOP_EXPRESSION + ") goto " + FUNCLABEL(binfo.jump_pc) + ";\n";
	}
	// else, exit binary translation
	// The number of instructions to increment depends on if branch-instruction-counting is enabled
	exit_function(PCRELS(instr.Btype.signed_imm()), true); // Bracket (NOTE: not actually ending the function)
}

template <int W>
void Emitter<W>::emit()
{
	this->add_mapping(this->pc(), this->func);
	add_code(func + "_start:;");
	auto next_pc = tinfo.basepc;

	for (int i = 0; i < tinfo.len; i++) {
		this->m_idx = i;
		this->instr = tinfo.instr[i];
		this->m_pc = next_pc;
		next_pc = this->m_pc + this->instr.length();

		// If the address is a return address or a global JAL target
		if (i > 0 && (mapping_labels.count(i) || tinfo.global_jump_locations.count(this->pc()))) {
			this->increment_counter_so_far();
			// Re-entry through the current function
			code.append(FUNCLABEL(this->pc()) + ":;\n");
			this->mappings.push_back({
				this->pc(), this->func
			});
		}
		// known jump locations
		else if (tinfo.jump_locations.count(this->pc()) || labels.count(i)) {
			this->increment_counter_so_far();
			code.append(FUNCLABEL(this->pc()) + ":;\n");
		}

		this->m_instr_counter += 1;

		// instruction generation
		switch (instr.opcode()) {
		case RV32I_LOAD:
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
			} else {
				// We don't care about where we are in the page when rd=0
				const auto temp = "tmp" + PCRELS(0);
				add_code("uint8_t " + temp + ";");
				this->memory_load<uint8_t>(temp, "volatile uint8_t", instr.Itype.rs1, instr.Itype.signed_imm());
				add_code("(void)" + temp + ";");
			} break;
		case RV32I_STORE:
			switch (instr.Stype.funct3) {
			case 0x0: // I8
				this->memory_store("int8_t", instr.Stype.rs1, instr.Stype.signed_imm(), from_reg(instr.Stype.rs2));
				break;
			case 0x1: // I16
				this->memory_store("int16_t", instr.Stype.rs1, instr.Stype.signed_imm(), from_reg(instr.Stype.rs2));
				break;
			case 0x2: // I32
				this->memory_store("int32_t", instr.Stype.rs1, instr.Stype.signed_imm(), from_reg(instr.Stype.rs2));
				break;
			case 0x3: // I64
				this->memory_store("int64_t", instr.Stype.rs1, instr.Stype.signed_imm(), from_reg(instr.Stype.rs2));
				break;
			default:
				UNKNOWN_INSTRUCTION();
			}
			break;
		case RV32I_BRANCH: {
			this->increment_counter_so_far();
			const auto offset = instr.Btype.signed_imm();
			uint64_t dest_pc = this->pc() + offset;
			uint64_t jump_pc = 0;
			uint64_t call_pc = 0;
			// goto branch: restarts function
			bool ge = dest_pc == this->begin_pc();
			// forward label: branch inside code block
			if (offset > 0 && dest_pc < this->end_pc()) {
				// forward label: future address
				labels.insert(dest_pc);
				jump_pc = dest_pc;
			} else if (tinfo.jump_locations.count(dest_pc)) {
				// existing jump location
				if (dest_pc >= this->begin_pc() && dest_pc < this->end_pc()) {
					jump_pc = dest_pc;
				}
			} else if (this->block_exists(dest_pc)) {
				call_pc = dest_pc;
			}
			switch (instr.Btype.funct3) {
			case 0x0: // EQ
				add_branch({ false, ge, jump_pc, call_pc }, " == ");
				break;
			case 0x1: // NE
				add_branch({ false, ge, jump_pc, call_pc }, " != ");
				break;
			case 0x2:
			case 0x3:
				UNKNOWN_INSTRUCTION();
				break;
			case 0x4: // LT
				add_branch({ true, ge, jump_pc, call_pc }, " < ");
				break;
			case 0x5: // GE
				add_branch({ true, ge, jump_pc, call_pc }, " >= ");
				break;
			case 0x6: // LTU
				add_branch({ false, ge, jump_pc, call_pc }, " < ");
				break;
			case 0x7: // GEU
				add_branch({ false, ge, jump_pc, call_pc }, " >= ");
				break;
			} } break;
		case RV32I_JALR: {
			// jump to register + immediate
			this->increment_counter_so_far();
			if (instr.Itype.rd != 0) {
				// NOTE: We need to remember RS1 because it can be clobbered by RD
				add_code(
					"{addr_t rs1 = " + from_reg(instr.Itype.rs1) + ";",
					to_reg(instr.Itype.rd) + " = " + PCRELS(4) + ";",
					"jump(cpu, rs1 + " + from_imm(instr.Itype.signed_imm()) + "); }"
				);
			} else {
				add_code(
					"jump(cpu, " + from_reg(instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");"
				);
			}
			exit_function("cpu->pc", true);
			} return;
		case RV32I_JAL: {
			this->increment_counter_so_far();
			if (instr.Jtype.rd != 0) {
				add_code(to_reg(instr.Jtype.rd) + " = " + PCRELS(4) + ";\n");
			}
			// XXX: mask off unaligned jumps - is this OK?
			const auto dest_pc = (this->pc() + instr.Jtype.jump_offset()) & ~address_t(ALIGN_MASK);
			// forward label: jump inside code block
			if (dest_pc >= this->begin_pc() && dest_pc < this->end_pc()) {
				// forward labels require creating future labels
				if (dest_pc > this->pc()) {
					labels.insert(dest_pc);
					add_code("goto " + FUNCLABEL(dest_pc) + ";");
				} else {
					// jump backwards: use counters
					add_code("if (" + LOOP_EXPRESSION + ") goto " + FUNCLABEL(dest_pc) + ";");
				}
				// .. if we run out of instructions, we must jump manually and exit:
			}
			// Because of forward jumps we can't end the function here
			exit_function(STRADDR(dest_pc));
			// Some blocks end with unconditional jumps
			if (no_labels_after_this()) {
				add_code("}");
				return;
			}
			if (instr.Jtype.rd != 0)
				this->add_reentry_next();
			} break;

		case RV32I_OP_IMM: {
			// NOP: Instruction without side-effect
			if (UNLIKELY(instr.Itype.rd == 0)) break;

			const auto dst = to_reg(instr.Itype.rd);
			const auto src = from_reg(instr.Itype.rs1);
			switch (instr.Itype.funct3) {
			case 0x0: // ADDI
				if (instr.Itype.signed_imm() == 0) {
					add_code(dst + " = " + src + ";");
				} else {
					emit_op(" + ", " += ", instr.Itype.rd, instr.Itype.rs1, from_imm(instr.Itype.signed_imm()));
				} break;
			case 0x1: // SLLI
				// SLLI: Logical left-shift 5/6-bit immediate
				switch (instr.Itype.imm) {
				case 0b011000000100: // SEXT.B
					add_code(
						dst + " = (saddr_t)(int8_t)" + src + ";");
					break;
				case 0b011000000101: // SEXT.H
					add_code(
						dst + " = (saddr_t)(int16_t)" + src + ";");
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
					if (instr.Itype.high_bits() == 0) { // SLLI
						emit_op(" << ", " <<= ", instr.Itype.rd, instr.Itype.rs1,
							std::to_string(instr.Itype.shift64_imm() & (XLEN-1)));
					} else if (instr.Itype.high_bits() == 0x280) {
						// BSETI: Bit-set immediate
						add_code(dst + " = " + src + " | (1UL << (" + std::to_string(instr.Itype.imm & (XLEN-1)) + "));");
					}
					else if (instr.Itype.high_bits() == 0x480) {
						// BCLRI: Bit-clear immediate
						add_code(dst + " = " + src + " & ~(1UL << (" + std::to_string(instr.Itype.imm & (XLEN-1)) + "));");
					}
					else if (instr.Itype.high_bits() == 0x680) {
						// BINVI: Bit-invert immediate
						add_code(dst + " = " + src + " ^ (1UL << (" + std::to_string(instr.Itype.imm & (XLEN-1)) + "));");
					} else {
						UNKNOWN_INSTRUCTION();
					}
				}
				break;
			case 0x2: // SLTI:
				// signed less than immediate
				add_code(
					dst + " = ((saddr_t)" + src + " < " + from_imm(instr.Itype.signed_imm()) + ") ? 1 : 0;");
				break;
			case 0x3: // SLTU:
				add_code(
					dst + " = (" + src + " < (unsigned) " + from_imm(instr.Itype.signed_imm()) + ") ? 1 : 0;");
				break;
			case 0x4: // XORI:
				emit_op(" ^ ", " ^= ", instr.Itype.rd, instr.Itype.rs1, from_imm(instr.Itype.signed_imm()));
				break;
			case 0x5: // SRLI / SRAI / ORC.B:
				if (instr.Itype.is_rori()) {
					// RORI: Rotate right immediate
					add_code(
					"{const unsigned shift = " + from_imm(instr.Itype.imm & (XLEN-1)) + ";\n",
						dst + " = (" + src + " >> shift) | (" + src + " << (XLEN - shift)); }"
					);
				} else if (instr.Itype.imm == 0x287) {
					// ORC.B: Bitwise OR-combine
					add_code(
						"for (unsigned i = 0; i < sizeof(addr_t); i++)",
						"	((char *)&" + dst + ")[i] = ((char *)&" + src + ")[i] ? 0xFF : 0x0;"
					);
				} else if (instr.Itype.is_rev8<sizeof(dst)>()) {
					// REV8: Byte-reverse register
					if constexpr (W == 4)
						add_code(dst + " = do_bswap32(" + src + ");");
					else
						add_code(dst + " = do_bswap64(" + src + ");");
				} else if (instr.Itype.high_bits() == 0x0) { // SRLI
					emit_op(" >> ", " >>= ", instr.Itype.rd, instr.Itype.rs1,
						std::to_string(instr.Itype.shift64_imm() & (XLEN-1)));
				} else if (instr.Itype.high_bits() == 0x400) { // SRAI: preserve the sign bit
					add_code(
						dst + " = (saddr_t)" + src + " >> (" + from_imm(instr.Itype.signed_imm()) + " & (XLEN-1));");
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
			} break;
		case RV32I_OP:
			if (UNLIKELY(instr.Rtype.rd == 0)) break;

			switch (instr.Rtype.jumptable_friendly_op()) {
			case 0x0: // ADD
				emit_op(" + ", " += ", instr.Rtype.rd, instr.Rtype.rs1, from_reg(instr.Rtype.rs2));
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
				emit_op(" | ", " |= ", instr.Rtype.rd, instr.Rtype.rs1, to_reg(instr.Rtype.rs2));
				break;
			case 0x7: // AND
				emit_op(" & ", " &= ", instr.Rtype.rd, instr.Rtype.rs1, to_reg(instr.Rtype.rs2));
				break;
			// extension RV32M / RV64M
			case 0x10: // MUL
				add_code(
					to_reg(instr.Rtype.rd) + " = (saddr_t)" + from_reg(instr.Rtype.rs1) + " * (saddr_t)" + from_reg(instr.Rtype.rs2) + ";");
				break;
			case 0x11: // MULH (signed x signed)
				add_code(
					(W == 4) ?
					to_reg(instr.Rtype.rd) + " = (uint64_t)((int64_t)(saddr_t)" + from_reg(instr.Rtype.rs1) + " * (int64_t)(saddr_t)" + from_reg(instr.Rtype.rs2) + ") >> 32u;" :
					"MUL128(&" + from_reg(instr.Rtype.rd) + ", " + from_reg(instr.Rtype.rs1) + ", " + from_reg(instr.Rtype.rs2) + ");"
				);
				break;
			case 0x12: // MULHSU (signed x unsigned)
				add_code(
					(W == 4) ?
					to_reg(instr.Rtype.rd) + " = (uint64_t)((int64_t)(saddr_t)" + from_reg(instr.Rtype.rs1) + " * (uint64_t)" + from_reg(instr.Rtype.rs2) + ") >> 32u;" :
					"MUL128(&" + from_reg(instr.Rtype.rd) + ", " + from_reg(instr.Rtype.rs1) + ", " + from_reg(instr.Rtype.rs2) + ");"
				);
				break;
			case 0x13: // MULHU (unsigned x unsigned)
				add_code(
					(W == 4) ?
					to_reg(instr.Rtype.rd) + " = ((uint64_t) " + from_reg(instr.Rtype.rs1) + " * (uint64_t)" + from_reg(instr.Rtype.rs2) + ") >> 32u;" :
					"MUL128(&" + from_reg(instr.Rtype.rd) + ", " + from_reg(instr.Rtype.rs1) + ", " + from_reg(instr.Rtype.rs2) + ");"
				);
				break;
			case 0x14: // DIV
				// division by zero is not an exception
				if constexpr (W == 8) {
					add_code(
						"if (LIKELY(" + from_reg(instr.Rtype.rs2) + " != 0)) {",
						"	if (LIKELY(!(" + from_reg(instr.Rtype.rs1) + " == -9223372036854775808ull && " + from_reg(instr.Rtype.rs2) + " == -1ull)))"
						"		" + from_reg(instr.Rtype.rd) + " = (int64_t)" + from_reg(instr.Rtype.rs1) + " / (int64_t)" + from_reg(instr.Rtype.rs2) + ";",
						"}");
				} else {
					add_code(
						"if (LIKELY(" + from_reg(instr.Rtype.rs2) + " != 0)) {",
						"	if (LIKELY(!(" + from_reg(instr.Rtype.rs1) + " == 2147483648 && " + from_reg(instr.Rtype.rs2) + " == 4294967295)))",
						"		" + from_reg(instr.Rtype.rd) + " = (int32_t)" + from_reg(instr.Rtype.rs1) + " / (int32_t)" + from_reg(instr.Rtype.rs2) + ";",
						"}");
				}
				break;
			case 0x15: // DIVU
				add_code(
					"if (LIKELY(" + from_reg(instr.Rtype.rs2) + " != 0))",
					to_reg(instr.Rtype.rd) + " = " + from_reg(instr.Rtype.rs1) + " / " + from_reg(instr.Rtype.rs2) + ";"
				);
				break;
			case 0x16: // REM
				if constexpr (W == 8) {
					add_code(
					"if (LIKELY(" + from_reg(instr.Rtype.rs2) + " != 0)) {",
					"	if (LIKELY(!(" + from_reg(instr.Rtype.rs1) + " == -9223372036854775808ull && " + from_reg(instr.Rtype.rs2) + " == -1ull)))",
					"		" + from_reg(instr.Rtype.rd) + " = (int64_t)" + from_reg(instr.Rtype.rs1) + " % (int64_t)" + from_reg(instr.Rtype.rs2) + ";",
					"}");
				} else {
					add_code(
					"if (LIKELY(" + from_reg(instr.Rtype.rs2) + " != 0)) {",
					"	if (LIKELY(!(" + from_reg(instr.Rtype.rs1) + " == 2147483648 && " + from_reg(instr.Rtype.rs2) + " == 4294967295)))",
					"		" + from_reg(instr.Rtype.rd) + " = (int32_t)" + from_reg(instr.Rtype.rs1) + " % (int32_t)" + from_reg(instr.Rtype.rs2) + ";",
					"}");
				}
				break;
			case 0x17: // REMU
				add_code(
				"if (LIKELY(" + from_reg(instr.Rtype.rs2) + " != 0))",
					to_reg(instr.Rtype.rd) + " = " + from_reg(instr.Rtype.rs1) + " % " + from_reg(instr.Rtype.rs2) + ";"
				);
				break;
			case 0x44: // ZEXT.H: Zero-extend 16-bit
				add_code(to_reg(instr.Rtype.rd) + " = (uint16_t)" + from_reg(instr.Rtype.rs1) + ";");
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
					"for (unsigned i = 0; i < XLEN-1; i++)",
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
				add_code(to_reg(instr.Rtype.rd) + " = " + to_reg(instr.Rtype.rs2) + " + (" + to_reg(instr.Rtype.rs1) + " << 1);");
				break;
			case 0x104: // SH2ADD
				add_code(to_reg(instr.Rtype.rd) + " = " + to_reg(instr.Rtype.rs2) + " + (" + to_reg(instr.Rtype.rs1) + " << 2);");
				break;
			case 0x106: // SH3ADD
				add_code(to_reg(instr.Rtype.rd) + " = " + to_reg(instr.Rtype.rs2) + " + (" + to_reg(instr.Rtype.rs1) + " << 3);");
				break;
			case 0x141: // BSET
				add_code(to_reg(instr.Rtype.rd) + " = " + to_reg(instr.Rtype.rs1) + " | (1UL << (" + to_reg(instr.Rtype.rs2) + " & (XLEN-1)));");
				break;
			case 0x142: // BCLR
				add_code(to_reg(instr.Rtype.rd) + " = " + to_reg(instr.Rtype.rs1) + " & ~(1UL << (" + to_reg(instr.Rtype.rs2) + " & (XLEN-1)));");
				break;
			case 0x143: // BINV
				add_code(to_reg(instr.Rtype.rd) + " = " + to_reg(instr.Rtype.rs1) + " ^ (1UL << (" + to_reg(instr.Rtype.rs2) + " & (XLEN-1)));");
				break;
			case 0x204: // XNOR
				add_code(to_reg(instr.Rtype.rd) + " = ~(" + to_reg(instr.Rtype.rs1) + " ^ " + to_reg(instr.Rtype.rs2) + ");");
				break;
			case 0x206: // ORN
				add_code(to_reg(instr.Rtype.rd) + " = (" + to_reg(instr.Rtype.rs1) + " | ~" + to_reg(instr.Rtype.rs2) + ");");
				break;
			case 0x207: // ANDN
				add_code(to_reg(instr.Rtype.rd) + " = (" + to_reg(instr.Rtype.rs1) + " & ~" + to_reg(instr.Rtype.rs2) + ");");
				break;
			case 0x245: // BEXT
				add_code(to_reg(instr.Rtype.rd) + " = (" + to_reg(instr.Rtype.rs1) + " >> (" + to_reg(instr.Rtype.rs2) + " & (XLEN-1))) & 1;");
				break;
			case 0x54: // MIN
				add_code(to_reg(instr.Rtype.rd) + " = ((saddr_t)" + to_reg(instr.Rtype.rs1) + " < (saddr_t)" + to_reg(instr.Rtype.rs2) + ") "
					" ? " + to_reg(instr.Rtype.rs1) + " : " + to_reg(instr.Rtype.rs2) + ";");
				break;
			case 0x55: // MINU
				add_code(to_reg(instr.Rtype.rd) + " = (" + to_reg(instr.Rtype.rs1) + " < " + to_reg(instr.Rtype.rs2) + ") "
					" ? " + to_reg(instr.Rtype.rs1) + " : " + to_reg(instr.Rtype.rs2) + ";");
				break;
			case 0x56: // MAX
				add_code(to_reg(instr.Rtype.rd) + " = ((saddr_t)" + to_reg(instr.Rtype.rs1) + " > (saddr_t)" + to_reg(instr.Rtype.rs2) + ") "
					" ? " + to_reg(instr.Rtype.rs1) + " : " + to_reg(instr.Rtype.rs2) + ";");
				break;
			case 0x57: // MAXU
				add_code(to_reg(instr.Rtype.rd) + " = (" + to_reg(instr.Rtype.rs1) + " > " + to_reg(instr.Rtype.rs2) + ") "
					" ? " + to_reg(instr.Rtype.rs1) + " : " + to_reg(instr.Rtype.rs2) + ";");
				break;
			case 0x301: // ROL: Rotate left
				add_code(
				"{const unsigned shift = " + from_reg(instr.Rtype.rs2) + " & (XLEN-1);\n",
					to_reg(instr.Rtype.rd) + " = (" + from_reg(instr.Rtype.rs1) + " << shift) | (" + from_reg(instr.Rtype.rs1) + " >> (XLEN - shift)); }"
				);
				break;
			case 0x305: // ROR: Rotate right
				add_code(
				"{const unsigned shift = " + from_reg(instr.Rtype.rs2) + " & (XLEN-1);\n",
					to_reg(instr.Rtype.rd) + " = (" + from_reg(instr.Rtype.rs1) + " >> shift) | (" + from_reg(instr.Rtype.rs1) + " << (XLEN - shift)); }"
				);
				break;
			default:
				//fprintf(stderr, "RV32I_OP: Unhandled function 0x%X\n",
				//		instr.Rtype.jumptable_friendly_op());
				UNKNOWN_INSTRUCTION();
			}
			break;
		case RV32I_LUI:
			if (UNLIKELY(instr.Utype.rd == 0))
				break;
			add_code(
				to_reg(instr.Utype.rd) + " = " + from_imm(instr.Utype.upper_imm()) + ";");
			break;
		case RV32I_AUIPC:
			if (UNLIKELY(instr.Utype.rd == 0))
				break;
			add_code(
				to_reg(instr.Utype.rd) + " = " + PCRELS(instr.Utype.upper_imm()) + ";");
			break;
		case RV32I_FENCE:
			break;
		case RV32I_SYSTEM:
			if (instr.Itype.funct3 == 0x0) {
				this->increment_counter_so_far();
				// System calls and EBREAK
				if (instr.Itype.imm < 2) {
					const auto syscall_reg =
						(instr.Itype.imm == 0) ? from_reg(REG_ECALL) : std::to_string(SYSCALL_EBREAK);
					this->restore_syscall_registers();
					code += "cpu->pc = " + PCRELS(0) + ";\n";
					code += "if (UNLIKELY(do_syscall(cpu, counter, max_counter, " + syscall_reg + "))) {\n"
						"  cpu->pc += 4; return (ReturnValues){counter, MAX_COUNTER(cpu)};}\n"; // Correct for +4 expectation outside of bintr
					code += "max_counter = MAX_COUNTER(cpu);\n"; // Restore max counter
					// Restore A0
					this->invalidate_register(REG_ARG0);
					this->potentially_reload_register(REG_ARG0);
					break;
				} if (instr.Itype.imm == 261 || instr.Itype.imm == 0x7FF) { // WFI / STOP
					code += "max_counter = 0;\n"; // Immediate stop PC + 4
					exit_function(PCRELS(4), true);
					return;
				} else {
					// Zero funct3, unknown imm: Don't exit
					code += "cpu->pc = " + PCRELS(0) + ";\n";
					code += "api.system(cpu, " + std::to_string(instr.whole) +");\n";
					break;
				}
			} else {
				// Non-zero funct3: CSR and other system functions
				code += "cpu->pc = " + PCRELS(0) + ";\n";
				code += "INS_COUNTER(cpu) = counter;\n"; // Reveal instruction counters
				code += "MAX_COUNTER(cpu) = max_counter;\n";
				code += "api.system(cpu, " + std::to_string(instr.whole) +");\n";
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
			switch (instr.Itype.funct3) {
			case 0x0:
				// ADDIW: Add sign-extended 12-bit immediate
				add_code(dst + " = " + SIGNEXTW + " (" + src + " + " + from_imm(instr.Itype.signed_imm()) + ");");
				break;
			case 0x1: // SLLI.W / SLLI.UW:
				if (instr.Itype.high_bits() == 0x000) {
					add_code(dst + " = " + SIGNEXTW + " (" + src + " << " + from_imm(instr.Itype.shift_imm()) + ");");
				} else if (instr.Itype.high_bits() == 0x080) {
					// SLLI.UW
					add_code(dst + " = ((addr_t)" + src + " << " + from_imm(instr.Itype.shift_imm()) + ");");
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
				if (instr.Itype.high_bits() == 0x0) { // SRLIW
					add_code(dst + " = " + SIGNEXTW + " (" + src + " >> " + from_imm(instr.Itype.shift_imm()) + ");");
				} else if (instr.Itype.high_bits() == 0x400) { // SRAIW: preserve the sign bit
					add_code(
						dst + " = (int32_t)" + src + " >> " + from_imm(instr.Itype.shift_imm()) + ";");
				} else if (instr.Itype.high_bits() == 0x600) { // RORIW
					add_code(
					"{const unsigned shift = " + from_imm(instr.Itype.imm) + " & 31;\n",
						dst + " = (int32_t)(" + src + " >> shift) | (" + src + " << (32 - shift)); }"
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
				// division by zero is not an exception
				add_code(
				"if (LIKELY(" + src2 + " != 0))",
				"if (LIKELY(!((int32_t)" + src1 + " == -2147483648 && (int32_t)" + src2 + " == -1)))",
				dst + " = " + SIGNEXTW + " ((int32_t)" + src1 + " / (int32_t)" + src2 + ");");
				break;
			case 0x15: // DIVUW
				add_code(
				"if (LIKELY(" + src2 + " != 0))",
				dst + " = " + SIGNEXTW + " (" + src1 + " / " + src2 + ");");
				break;
			case 0x16: // REMW
				add_code(
				"if (LIKELY(" + src2 + " != 0))",
				"if (LIKELY(!((int32_t)" + src1 + " == -2147483648 && (int32_t)" + src2 + " == -1)))",
				dst + " = " + SIGNEXTW + " ((int32_t)" + src1 + " % (int32_t)" + src2 + ");");
				break;
			case 0x17: // REMUW
				add_code(
				"if (LIKELY(" + src2 + " != 0))",
				dst + " = " + SIGNEXTW + " (" + src1 + " % " + src2 + ");");
				break;
			case 0x40: // ADD.UW
				add_code(dst + " = " + from_reg(instr.Rtype.rs2) + " + " + src1 + ";");
				break;
			case 0x44: // ZEXT.H (imm=0x40):
				add_code(dst + " = (uint16_t)(" + src1 + ");");
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
					to_reg(instr.Rtype.rd) + " = (int32_t)(" + from_reg(instr.Rtype.rs1) + " << shift) | (" + from_reg(instr.Rtype.rs1) + " >> (32 - shift)); }"
				);
				break;
			case 0x305: // RORW: Rotate right (32-bit)
				add_code(
				"{const unsigned shift = " + from_reg(instr.Rtype.rs2) + " & 31;\n",
					to_reg(instr.Rtype.rd) + " = (int32_t)(" + from_reg(instr.Rtype.rs1) + " >> shift) | (" + from_reg(instr.Rtype.rs1) + " << (32 - shift)); }"
				);
				break;
			default:
				UNKNOWN_INSTRUCTION();
			}
			} break;
		case RV32F_LOAD: {
			const rv32f_instruction fi{instr};
			switch (fi.Itype.funct3) {
			case 0x2: // FLW
				this->memory_load<uint32_t>(from_fpreg(fi.Itype.rd) + ".i32[0]", "uint32_t", fi.Itype.rs1, fi.Itype.signed_imm());
				code += from_fpreg(fi.Itype.rd) + ".i32[1] = 0;\n";
				break;
			case 0x3: // FLD
				this->memory_load<uint64_t>(from_fpreg(fi.Itype.rd) + ".i64", "uint64_t", fi.Itype.rs1, fi.Itype.signed_imm());
				break;
#ifdef RISCV_EXT_VECTOR
			case 0x6: { // VLE32
				const rv32v_instruction vi { instr };
				this->memory_load<VectorLane>(from_rvvreg(vi.VLS.vd), "VectorLane", vi.VLS.rs1, 0);
				break;
			}
#endif
			default:
				code += "api.execute(cpu, " + std::to_string(instr.whole) + ");\n";
				break;
			}
			} break;
		case RV32F_STORE: {
			const rv32f_instruction fi{instr};
			switch (fi.Itype.funct3) {
			case 0x2: // FSW
				this->memory_store("int32_t", fi.Stype.rs1, fi.Stype.signed_imm(), from_fpreg(fi.Stype.rs2) + ".i32[0]");
				break;
			case 0x3: // FSD
				this->memory_store("int64_t", fi.Stype.rs1, fi.Stype.signed_imm(), from_fpreg(fi.Stype.rs2) + ".i64");
				break;
#ifdef RISCV_EXT_VECTOR
			case 0x6: { // VSE32
				const rv32v_instruction vi { instr };
				this->memory_store("VectorLane", vi.VLS.rs1, 0, from_rvvreg(vi.VLS.vd));
				break;
			}
#endif
			default:
				code += "api.execute(cpu, " + std::to_string(instr.whole) + ");\n";
				break;
			}
			} break;
		case RV32F_FMADD:
		case RV32F_FMSUB:
		case RV32F_FNMADD:
		case RV32F_FNMSUB: {
			const rv32f_instruction fi{instr};
			const auto dst = from_fpreg(fi.R4type.rd);
			const auto rs1 = from_fpreg(fi.R4type.rs1);
			const auto rs2 = from_fpreg(fi.R4type.rs2);
			const auto rs3 = from_fpreg(fi.R4type.rs3);
			const std::string sign = (instr.opcode() == RV32F_FNMADD || instr.opcode() == RV32F_FNMSUB) ? "-" : "";
			const std::string add = (instr.opcode() == RV32F_FMSUB || instr.opcode() == RV32F_FNMSUB) ? " - " : " + ";
			if (fi.R4type.funct2 == 0x0) { // float32
				code += "set_fl(&" + dst + ", " + sign + "(" + rs1 + ".f32[0] * " + rs2 + ".f32[0]" + add + rs3 + ".f32[0]));\n";
			} else if (fi.R4type.funct2 == 0x1) { // float64
				code += "set_dbl(&" + dst + ", " + sign + "(" + rs1 + ".f64 * " + rs2 + ".f64" + add + rs3 + ".f64));\n";
			} else {
				UNKNOWN_INSTRUCTION();
			}
			} break;
		case RV32F_FPFUNC: {
			const rv32f_instruction fi{instr};
			const auto dst = from_fpreg(fi.R4type.rd);
			const auto rs1 = from_fpreg(fi.R4type.rs1);
			const auto rs2 = from_fpreg(fi.R4type.rs2);
			if (fi.R4type.funct2 < 0x2) { // fp32 / fp64
			switch (instr.fpfunc()) {
			case RV32F__FEQ_LT_LE:
				if (UNLIKELY(fi.R4type.rd == 0)) {
					UNKNOWN_INSTRUCTION();
					break;
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
				} break;
			case RV32F__FMIN_MAX:
				switch (fi.R4type.funct3 | (fi.R4type.funct2 << 4)) {
				case 0x0: // FMIN.S
					code += "set_fl(&" + dst + ", fminf(" + rs1 + ".f32[0], " + rs2 + ".f32[0]));\n";
					break;
				case 0x1: // FMAX.S
					code += "set_fl(&" + dst + ", fmaxf(" + rs1 + ".f32[0], " + rs2 + ".f32[0]));\n";
					break;
				case 0x10: // FMIN.D
					code += "set_dbl(&" + dst + ", fmin(" + rs1 + ".f64, " + rs2 + ".f64));\n";
					break;
				case 0x11: // FMAX.D
					code += "set_dbl(&" + dst + ", fmax(" + rs1 + ".f64, " + rs2 + ".f64));\n";
					break;
				default:
					UNKNOWN_INSTRUCTION();
				} break;
			case RV32F__FADD:
			case RV32F__FSUB:
			case RV32F__FMUL:
			case RV32F__FDIV: {
				std::string fop = " + ";
				if (instr.fpfunc() == RV32F__FSUB) fop = " - ";
				else if (instr.fpfunc() == RV32F__FMUL) fop = " * ";
				else if (instr.fpfunc() == RV32F__FDIV) fop = " / ";
				if (fi.R4type.funct2 == 0x0) { // fp32
					code += "set_fl(&" + dst + ", " + rs1 + ".f32[0]" + fop + rs2 + ".f32[0]);\n";
				} else { // fp64
					code += "set_dbl(&" + dst + ", " + rs1 + ".f64" + fop + rs2 + ".f64);\n";
				}
				} break;
			case RV32F__FSQRT:
				if (fi.R4type.funct2 == 0x0) { // fp32
					code += "set_fl(&" + dst + ", api.sqrtf32(" + rs1 + ".f32[0]));\n";
				} else { // fp64
					code += "set_dbl(&" + dst + ", api.sqrtf64(" + rs1 + ".f64));\n";
				}
				break;
			case RV32F__FSGNJ_NX:
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
				if (fi.R4type.funct2 == 0x0) {
					code += "set_fl(&" + dst + ", " + rs1 + ".f64);\n";
				} else if (fi.R4type.funct2 == 0x1) {
					code += "set_dbl(&" + dst + ", " + rs1 + ".f32[0]);\n";
				} else {
					UNKNOWN_INSTRUCTION();
				} break;
			case RV32F__FCVT_SD_W: {
				const std::string sign((fi.R4type.rs2 == 0x0) ? "(saddr_t)" : "");
				if (fi.R4type.funct2 == 0x0) {
					code += "set_fl(&" + dst + ", " + sign + from_reg(fi.R4type.rs1) + ");\n";
				} else if (fi.R4type.funct2 == 0x1) {
					code += "set_dbl(&" + dst + ", " + sign + from_reg(fi.R4type.rs1) + ");\n";
				} else {
					UNKNOWN_INSTRUCTION();
				}
				} break;
			case RV32F__FCVT_W_SD: {
				const std::string sign((fi.R4type.rs2 == 0x0) ? "(int32_t)" : "(uint32_t)");
				if (fi.R4type.rd != 0 && fi.R4type.funct2 == 0x0) {
					code += to_reg(fi.R4type.rd) + " = " + sign + rs1 + ".f32[0];\n";
				} else if (fi.R4type.rd != 0 && fi.R4type.funct2 == 0x1) {
					code += to_reg(fi.R4type.rd) + " = " + sign + rs1 + ".f64;\n";
				} else {
					UNKNOWN_INSTRUCTION();
				}
				} break;
			case RV32F__FMV_W_X:
				if (fi.R4type.funct2 == 0x0) {
					code += "load_fl(&" + dst + ", " + from_reg(fi.R4type.rs1) + ");\n";
				} else if (W == 8 && fi.R4type.funct2 == 0x1) {
					code += "load_dbl(&" + dst + ", " + from_reg(fi.R4type.rs1) + ");\n";
				} else {
					UNKNOWN_INSTRUCTION();
				} break;
			case RV32F__FMV_X_W:
				if (fi.R4type.funct3 == 0x0) {
					if (fi.R4type.rd != 0 && fi.R4type.funct2 == 0x0) {
						code += to_reg(fi.R4type.rd) + " = " + rs1 + ".i32[0];\n";
					} else if (W == 8 && fi.R4type.rd != 0 && fi.R4type.funct2 == 0x1) { // 64-bit only
						code += to_reg(fi.R4type.rd) + " = " + rs1 + ".i64;\n";
					} else {
						UNKNOWN_INSTRUCTION();
					}
				} else { // FPCLASSIFY etc.
					code += "api.execute(cpu, " + std::to_string(instr.whole) + ");\n";
				} break;
			} // fpfunc
			} else UNKNOWN_INSTRUCTION();
			} break; // RV32F_FPFUNC
		case RV32A_ATOMIC: // General handler for atomics
			code += "api.execute(cpu, " + std::to_string(instr.whole) + ");\n";
			break;
		case RV32V_OP: {   // General handler for vector instructions
#ifdef RISCV_EXT_VECTOR
			const rv32v_instruction vi{instr};
			switch (instr.vwidth()) {
			case 0x1: // OPF.VV
				switch (vi.OPVV.funct6)
				{
				case 0b000000: // VFADD.VV
					code +=
						"for (unsigned i = 0; i < RISCV_EXT_VECTOR/4; i++) {\n"
						"  " + from_rvvreg(vi.OPVV.vd) + ".f32[i] = " + from_rvvreg(vi.OPVV.vs1) + ".f32[i] + " + from_rvvreg(vi.OPVV.vs2) + ".f32[i];\n"
						"}\n";
					break;
				case 0b100100: // VFMUL.VV
					code +=
						"for (unsigned i = 0; i < RISCV_EXT_VECTOR/4; i++) {\n"
						"  " + from_rvvreg(vi.OPVV.vd) + ".f32[i] = " + from_rvvreg(vi.OPVV.vs1) + ".f32[i] * " + from_rvvreg(vi.OPVV.vs2) + ".f32[i];\n"
						"}\n";
					break;
				default:
					code += "api.execute(cpu, " + std::to_string(instr.whole) + ");\n";
				}
				break;
			case 0x5: { // OPF.VF
				const std::string scalar = "scalar" + PCRELS(0);
				switch (vi.OPVV.funct6)
				{
				case 0b000000: // VFADD.VF
					code +=
						"const float " + scalar + " = " + from_fpreg(vi.OPVV.vs1) + ".f32[0];\n"
						"for (unsigned i = 0; i < RISCV_EXT_VECTOR/4; i++) {\n"
						"  " + from_rvvreg(vi.OPVV.vd) + ".f32[i] = " + from_rvvreg(vi.OPVV.vs2) + ".f32[i] + " + scalar + ";\n"
						"}\n";
					break;
				case 0b100100: // VFMUL.VF
					code +=
						"const float " + scalar + " = " + from_fpreg(vi.OPVV.vs1) + ".f32[0];\n"
						"for (unsigned i = 0; i < RISCV_EXT_VECTOR/4; i++) {\n"
						"  " + from_rvvreg(vi.OPVV.vd) + ".f32[i] = " + from_rvvreg(vi.OPVV.vs2) + ".f32[i] * " + scalar + ";\n"
						"}\n";
					break;
				default:
					code += "api.execute(cpu, " + std::to_string(instr.whole) + ");\n";
				}
				break;
			}
			default:
				code += "api.execute(cpu, " + std::to_string(instr.whole) + ");\n";
			}
			break;
#else
			code += "api.execute(cpu, " + std::to_string(instr.whole) + ");\n";
			break;
#endif
		}
		default:
			code += "api.execute(cpu, " + std::to_string(instr.whole) + ");\n";
		}
	}
	// If the function ends with an unimplemented instruction,
	// we must gracefully finish, setting new PC and incrementing IC
	this->increment_counter_so_far();
	exit_function(STRADDR(this->end_pc()), true);
}

template <int W>
std::vector<TransMapping<W>>
CPU<W>::emit(std::string& code, const TransInfo<W>& tinfo) const
{
	Emitter<W> e(const_cast<CPU<W>&>(*this), tinfo);
	e.emit();

	// Function header
	code += "static ReturnValues " + e.get_func() + "(CPU* cpu, uint64_t counter, uint64_t max_counter, addr_t pc) {\n";

	// Extra function entries
	if (e.get_mappings().size() > 1)
	{
		code += "switch (pc) {\n";
		code += "case " + std::to_string(tinfo.basepc) + ": goto " + e.get_func() + "_start;\n";
		for (size_t idx = 1; idx < e.get_mappings().size(); idx++) {
			auto& entry = e.get_mappings().at(idx);
			const auto label = funclabel<W>(e.get_func(), entry.addr);
			code += "case " + std::to_string(entry.addr) + ": goto " + label + ";\n";
		}
		//code += "default: api.exception(cpu, 3);\n";
		code += "}\n";
	}

	// Function GPRs
	for (size_t reg = 1; reg < 32; reg++) {
		if (e.get_gpr_exists()[reg]) {
			code += "addr_t " + e.loaded_regname(reg) + " = cpu->r[" + std::to_string(reg) + "];\n";
		}
	}

	// Function code
	code += e.get_code();

	return std::move(e.get_mappings());
}

#ifdef RISCV_32I
template std::vector<TransMapping<4>> CPU<4>::emit(std::string&, const TransInfo<4>&) const;
#endif
#ifdef RISCV_64I
template std::vector<TransMapping<8>> CPU<8>::emit(std::string&, const TransInfo<8>&) const;
#endif
} // riscv
