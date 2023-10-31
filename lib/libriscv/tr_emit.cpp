#include "machine.hpp"
#include "instruction_list.hpp"
#include "rv32i_instr.hpp"
#include "rvfd.hpp"
#include "tr_types.hpp"
#ifdef RISCV_EXT_VECTOR
#include "rvv.hpp"
#endif

#define PCRELA(x) ((address_t) (tinfo.basepc + index() * 4 + (x)))
#define PCRELS(x) std::to_string(PCRELA(x)) + "UL"
#define INSTRUCTION_COUNT(i) ("c + " + std::to_string(i))
#define ILLEGAL_AND_EXIT() { code += "api.exception(cpu, ILLEGAL_OPCODE);\nUNREACHABLE();\n"; }

namespace riscv {
static const std::string LOOP_EXPRESSION = "c < local_max_insn";

struct BranchInfo {
	bool sign;
	bool goto_enabled;
	int jump_label; // destination index, 0 when unused
};

template <int W>
struct Emitter
{
	static constexpr bool CACHED_REGISTERS = false;
	using address_t = address_type<W>;

	Emitter(CPU<W>& c, const std::string& pfunc, const TransInstr<W>* pip, const TransInfo<W>& ptinfo)
		: cpu(c), func(pfunc), ip(pip), tinfo(ptinfo) {}

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
	void exit_function(bool add_bracket = false)
	{
		if constexpr (CACHED_REGISTERS) {
			this->restore_all_registers();
		}
		add_code("return;");
		if (add_bracket) add_code("}");
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
	std::string memory_load(std::string type, int reg, int32_t imm)
	{
		const std::string data = "rpage" + PCRELS(0);
		std::string cast;
		if constexpr (std::is_signed_v<T>) {
			cast = "(saddr_t)";
		}

		if (reg == REG_GP && tinfo.gp != 0x0 && cpu.machine().memory.uses_memory_arena())
		{
			/* XXX: Check page permissions */
			const address_t absolute_vaddr = tinfo.gp + imm;
			if (absolute_vaddr + sizeof(T) <= this->cpu.machine().memory.memory_arena_size()) {
				add_code("const char* " + data + " = &arena_base[" + std::to_string(absolute_vaddr) + "];");
			}
			return cast + "*(" + type + "*)" + data;
		}

		const auto address = from_reg(reg) + " + " + from_imm(imm);
		if (cpu.machine().memory.uses_memory_arena()) {
			add_code(
				"const char* " + data + ";",
				"if (" + address + " < arena_size)",
				data + " = &arena_base[(" + address + ") & ~0xFFFLL];",
				"else",
				data + " = api.mem_ld(cpu, PAGENO(" + address + "));");
		} else {
			add_code("const char* " + data + " = api.mem_ld(cpu, PAGENO(" + address + "));");
		}
		return cast + "*(" + type + "*)&" + data + "[PAGEOFF(" + address + ")]";
	}
	void memory_store(std::string type, int reg, int32_t imm, std::string value)
	{
		const std::string data = "wpage" + PCRELS(0);

		if (reg == REG_GP && tinfo.gp != 0x0 && cpu.machine().memory.uses_memory_arena())
		{
			/* XXX: Check page permissions */
			const address_t absolute_vaddr = tinfo.gp + imm;
			if (absolute_vaddr + 8 <= this->cpu.machine().memory.memory_arena_size()) {
				add_code("char* " + data + " = &arena_base[" + std::to_string(absolute_vaddr) + "];");
			}
			add_code("*(" + type + "*)" + data + " = " + value + ";");
			return;
		}

		const auto address = from_reg(reg) + " + " + from_imm(imm);
		if (cpu.machine().memory.uses_memory_arena()) {
			add_code(
				"if (" + address + " < arena_size)",
				"  *(" + type + "*)&arena_base[" + address + "] = " + value + ";",
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

	size_t index() const noexcept { return this->m_idx; }
	address_t pc() const noexcept { return this->m_pc; }
	void emit();

private:
	std::string code;
	CPU<W>& cpu;
	size_t m_idx = 0;
	address_t m_pc = 0x0;
	rv32i_instruction instr;

	std::array<bool, 32> gprs {};
	std::array<bool, 32> gpr_exists {};

	const std::string& func;
	const TransInstr<W>* ip;
	const TransInfo<W>& tinfo;

	std::set<unsigned> labels;
	std::set<address_t> pagedata;
};

#define FUNCLABEL(i)  (func + "_" + std::to_string(i))
template <int W>
inline void Emitter<W>::add_branch(const BranchInfo& binfo, const std::string& op)
{
	using address_t = address_type<W>;
	if (binfo.sign == false)
		code += "if (" + from_reg(instr.Btype.rs1) + op + from_reg(instr.Btype.rs2) + ") {\n";
	else
		code += "if ((saddr_t)" + from_reg(instr.Btype.rs1) + op + " (saddr_t)" + from_reg(instr.Btype.rs2) + ") {\n";
	if (binfo.goto_enabled) {
		// this is a jump back to the start of the function
		code += "c += " + std::to_string(index()) + "; if (" + LOOP_EXPRESSION + ") goto " + func + "_start;\n";
	} else if (binfo.jump_label > 0) {
		// forward jump to label (from absolute index)
		code += "c += " + std::to_string(index()) + "; if (" + LOOP_EXPRESSION + ") ";
		code += "goto " + FUNCLABEL(binfo.jump_label) + ";\n";
		// else, exit binary translation
	}
	if (PCRELA(instr.Btype.signed_imm()) & 0x3)
	{
		code +=
			"api.exception(cpu, " + std::to_string(MISALIGNED_INSTRUCTION) + ");\n";
	}
	else
	{
		// The number of instructions to increment depends on if branch-instruction-counting is enabled
		code += 
			"*cur_insn = c; "
			"cpu->pc = " + PCRELS(instr.Btype.signed_imm() - 4) + ";\n";
		exit_function(true); // Bracket
	}
}

template <int W>
void Emitter<W>::emit()
{
	static constexpr unsigned XLEN = W * 8u;
	static const std::string SIGNEXTW = "(saddr_t) (int32_t)";
	add_code(func + "_start:;");

	for (int i = 0; i < tinfo.len; i++) {
		this->m_idx = i;
		this->instr = rv32i_instruction {ip[i].instr};
		this->m_pc = tinfo.basepc + index() * 4;

		// known jump locations
		if (tinfo.jump_locations.count(this->pc())) {
			code.append(FUNCLABEL(i) + ":;\n");
		}
		else if (labels.count(i))
		{ // forward branches (empty statement)
			code.append(FUNCLABEL(i) + ":;\n");
		}
		// instruction generation
		switch (instr.opcode()) {
		case RV32I_LOAD:
			if (instr.Itype.rd != 0) {
			switch (instr.Itype.funct3) {
			case 0x0: // I8
				add_code(from_reg(instr.Itype.rd) + " = " + this->memory_load<int8_t>("int8_t", instr.Itype.rs1, instr.Itype.signed_imm()) + ";");
				break;
			case 0x1: // I16
				add_code(from_reg(instr.Itype.rd) + " = " + this->memory_load<int16_t>("int16_t", instr.Itype.rs1, instr.Itype.signed_imm()) + ";");
				break;
			case 0x2: // I32
				add_code(from_reg(instr.Itype.rd) + " = " + this->memory_load<int32_t>("int32_t", instr.Itype.rs1, instr.Itype.signed_imm()) + ";");
				break;
			case 0x3: // I64
				add_code(from_reg(instr.Itype.rd) + " = " + this->memory_load<int64_t>("int64_t", instr.Itype.rs1, instr.Itype.signed_imm()) + ";");
				break;
			case 0x4: // U8
				add_code(from_reg(instr.Itype.rd) + " = " + this->memory_load<uint8_t>("uint8_t", instr.Itype.rs1, instr.Itype.signed_imm()) + ";");
				break;
			case 0x5: // U16
				add_code(from_reg(instr.Itype.rd) + " = " + this->memory_load<uint16_t>("uint16_t", instr.Itype.rs1, instr.Itype.signed_imm()) + ";");
				break;
			case 0x6: // U32
				add_code(from_reg(instr.Itype.rd) + " = " + this->memory_load<uint32_t>("uint32_t", instr.Itype.rs1, instr.Itype.signed_imm()) + ";");
				break;
			default:
				ILLEGAL_AND_EXIT();
			}
			} else {
				// We don't care about where we are in the page when rd=0
				add_code("(void)" + this->memory_load<int8_t>("int8_t", instr.Itype.rs1, instr.Itype.signed_imm()) + ";");
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
				ILLEGAL_AND_EXIT();
			}
			break;
		case RV32I_BRANCH: {
			const auto offset = instr.Btype.signed_imm() / 4;
			// goto branch: restarts function
			bool ge = tinfo.has_branch && (offset == -(long) i);
			// forward label: branch inside code block
			int fl = 0;
			if (offset > 0 && i+offset < tinfo.len) {
				// forward label: future address
				fl = i+offset;
				labels.insert(fl);
			} else if (tinfo.jump_locations.count(PCRELA(offset * 4))) {
				// forward label: existing jump location
				const int dstidx = i + offset;
				if (dstidx > 0 && dstidx < tinfo.len) {
					fl = dstidx;
				}
			}
			switch (instr.Btype.funct3) {
			case 0x0: // EQ
				add_branch({ false, ge, fl }, " == ");
				break;
			case 0x1: // NE
				add_branch({ false, ge, fl }, " != ");
				break;
			case 0x2:
			case 0x3:
				ILLEGAL_AND_EXIT();
				break;
			case 0x4: // LT
				add_branch({ true, ge, fl }, " < ");
				break;
			case 0x5: // GE
				add_branch({ true, ge, fl }, " >= ");
				break;
			case 0x6: // LTU
				add_branch({ false, ge, fl }, " < ");
				break;
			case 0x7: // GEU
				add_branch({ false, ge, fl }, " >= ");
				break;
			} } break;
		case RV32I_JALR: {
			// jump to register + immediate
			// NOTE: We need to remember RS1 because it can be clobbered by RD
			add_code("{addr_t jrs1 = " + from_reg(instr.Itype.rs1) + ";");
			if (instr.Itype.rd != 0) {
				add_code(to_reg(instr.Itype.rd) + " = " + PCRELS(4) + ";");
			}
			add_code(
				"*cur_insn = c + " + std::to_string(i) + ";\n"
				"jump(cpu, jrs1 + " + from_imm(instr.Itype.signed_imm()) + " - 4); }");
			exit_function(true);
			} return;
		case RV32I_JAL: {
			if (instr.Jtype.rd != 0) {
				add_code(to_reg(instr.Jtype.rd) + " = " + PCRELS(4) + ";\n");
			}
			// forward label: jump inside code block
			const auto offset = instr.Jtype.jump_offset() / 4;
			int fl = i+offset;
			if (std::abs(offset * 4) < 128 && fl > 0 && fl < tinfo.len) {
				// forward labels require creating future labels
				if (fl > i)
					labels.insert(fl);
				// this is a jump back to the start of the function
				add_code("c += " + std::to_string(i) + "; if (" + LOOP_EXPRESSION + ") goto " + FUNCLABEL(fl) + ";");
				// if we run out of instructions, we must exit:
				add_code(
					"*cur_insn = c;\n"
					"jump(cpu, " + PCRELS(instr.Jtype.jump_offset() - 4) + ");\n");
				exit_function();
			} else {
				// Because of forward jumps we can't end the function here
				add_code(
					"*cur_insn = c + " + std::to_string(i) + ";\n"
					"jump(cpu, " + PCRELS(instr.Jtype.jump_offset() - 4) + ");\n");
				exit_function();
			}
			if (no_labels_after_this()) {
				add_code("}");
				return;
			} } break;
		case RV32I_OP_IMM: {
			// NOP
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
							dst + " = " + src + " ? clz(" + src + ") : XLEN;");
					else
						add_code(
							dst + " = " + src + " ? clzl(" + src + ") : XLEN;");
					break;
				case 0b011000000001: // CTZ
					if constexpr (W == 4)
						add_code(
							dst + " = " + src + " ? __builtin_ctz(" + src + ") : XLEN;");
					else
						add_code(
							dst + " = " + src + " ? __builtin_ctzl(" + src + ") : XLEN;");
					break;
				case 0b011000000010: // CPOP
					if constexpr (W == 4)
						add_code(
							dst + " = __builtin_popcount(" + src + ");");
					else
						add_code(
							dst + " = __builtin_popcount(" + src + ");");
					break;
				default:
					emit_op(" << ", " <<= ", instr.Itype.rd, instr.Itype.rs1,
						std::to_string(instr.Itype.shift64_imm() & (XLEN-1)));
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
				} else if (LIKELY(!instr.Itype.is_srai())) {
					emit_op(" >> ", " >>= ", instr.Itype.rd, instr.Itype.rs1,
						std::to_string(instr.Itype.shift64_imm() & (XLEN-1)));
				} else { // SRAI: preserve the sign bit
					add_code(
						dst + " = (saddr_t)" + src + " >> (" + from_imm(instr.Itype.signed_imm()) + " & (XLEN-1));");
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
				if (instr.Rtype.funct7 == 0x0) {
					emit_op(" ^ ", " ^= ", instr.Rtype.rd, instr.Rtype.rs1, from_reg(instr.Rtype.rs2));
				} else if (instr.Rtype.funct7 == 0x4) {
					// ZEXT.H: Zero-extend 16-bit
					add_code(
						to_reg(instr.Rtype.rd) + " = uint16_t(" + from_reg(instr.Rtype.rs1) + ");");
				}
				break;
			case 0x5: // SRL / ROR
				if (instr.Itype.high_bits() == 0x0) {
					add_code(
						to_reg(instr.Rtype.rd) + " = " + from_reg(instr.Rtype.rs1) + " >> (" + from_reg(instr.Rtype.rs2) + " & (XLEN-1));");
				}
				else if (instr.Itype.is_rori()) {
					// ROR: Rotate right
					add_code(
					"{const unsigned shift = " + from_reg(instr.Rtype.rs2) + " & (XLEN-1);\n",
						to_reg(instr.Rtype.rd) + " = (" + from_reg(instr.Rtype.rs1) + " >> shift) | (" + from_reg(instr.Rtype.rs1) + " << (XLEN - shift)); }"
					);
				}
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
			case 0x102: // SH1ADD
				add_code(to_reg(instr.Rtype.rd) + " = " + to_reg(instr.Rtype.rs2) + " + (" + to_reg(instr.Rtype.rs1) + " << 1);");
				break;
			case 0x104: // SH2ADD
				add_code(to_reg(instr.Rtype.rd) + " = " + to_reg(instr.Rtype.rs2) + " + (" + to_reg(instr.Rtype.rs1) + " << 2);");
				break;
			case 0x106: // SH3ADD
				add_code(to_reg(instr.Rtype.rd) + " = " + to_reg(instr.Rtype.rs2) + " + (" + to_reg(instr.Rtype.rs1) + " << 3);");
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
			case 0x301: // ROL
				add_code(
				"{const unsigned shift = " + from_reg(instr.Rtype.rs2) + " & (XLEN-1);\n",
					to_reg(instr.Rtype.rd) + " = (" + from_reg(instr.Rtype.rs1) + " << shift) | (" + from_reg(instr.Rtype.rs1) + " >> (XLEN - shift)); }"
				);
				break;
			default:
				//fprintf(stderr, "RV32I_OP: Unhandled function 0x%X\n",
				//		instr.Rtype.jumptable_friendly_op());
				ILLEGAL_AND_EXIT();
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
				if (instr.Itype.imm == 0) {
					code += "cpu->pc = " + PCRELS(0) + "; "
							"*cur_insn = c;\n";
					const auto syscall_reg = from_reg(REG_ECALL);
					this->restore_syscall_registers();
					code += "if (UNLIKELY(api.syscall(cpu, " + syscall_reg + "))) {\nreturn;}\n";
					code += "local_max_insn = *max_insn;\n";
					// Restore A0
					this->invalidate_register(REG_ARG0);
					this->potentially_reload_register(REG_ARG0);
					break;
				} if (instr.Itype.imm == 1) {
					code += "cpu->pc = " + PCRELS(0) + "; "
							"*cur_insn = c;\n";
					code += "api.ebreak(cpu);\n";
					exit_function();
					break;
				} if (instr.Itype.imm == 261) {
					code += "cpu->pc = " + PCRELS(0) + "; "
							"*cur_insn = c;\n";
					code += "*max_insn = 0;\n";
					exit_function();
					break;
				} else {
					code += "api.system(cpu, " + std::to_string(instr.whole) +");\n";
					break;
				}
			} else {
				code += "api.system(cpu, " + std::to_string(instr.whole) +");\n";
			} break;
		case RV64I_OP_IMM32: {
			if (UNLIKELY(instr.Itype.rd == 0))
				break;
			const auto dst = to_reg(instr.Itype.rd);
			const auto src = "(uint32_t)" + from_reg(instr.Itype.rs1);
			switch (instr.Itype.funct3) {
			case 0x0:
				// ADDIW: Add sign-extended 12-bit immediate
				add_code(dst + " = " + SIGNEXTW + " (" + src + " + " + from_imm(instr.Itype.signed_imm()) + ");");
				break;
			case 0x1: // SLLIW:
				add_code(dst + " = " + SIGNEXTW + " (" + src + " << " + from_imm(instr.Itype.shift_imm()) + ");");
				break;
			case 0x5: // SRLIW / SRAIW:
				if (LIKELY(!instr.Itype.is_srai())) {
					add_code(dst + " = " + SIGNEXTW + " (" + src + " >> " + from_imm(instr.Itype.shift_imm()) + ");");
				} else { // SRAIW: preserve the sign bit
					add_code(
						dst + " = (int32_t)" + src + " >> " + from_imm(instr.Itype.shift_imm()) + ";");
				}
				break;
			default:
				ILLEGAL_AND_EXIT();
			}
			} break;
		case RV64I_OP32: {
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
			case 0x5: // SRLW / RORW
				if (instr.Itype.high_bits() == 0x0) {
					add_code(dst + " = " + SIGNEXTW + " (" + src1 + " >> (" + src2 + " & 0x1F));");
				}
				else if (instr.Itype.is_rori()) {
					// RORW: Rotate right (32-bit)
					add_code(
					"{const unsigned shift = " + from_reg(instr.Rtype.rs2) + " & 31;\n",
						to_reg(instr.Rtype.rd) + " = (int32_t)(" + from_reg(instr.Rtype.rs1) + " >> shift) | (" + from_reg(instr.Rtype.rs1) + " << (32 - shift)); }"
					);
				}
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
				add_code(dst + " = " + from_reg(instr.Rtype.rs2) + " + (" + src1 + " << 1);");
				break;
			case 0x104: // SH2ADD.UW
				add_code(dst + " = " + from_reg(instr.Rtype.rs2) + " + (" + src1 + " << 2);");
				break;
			case 0x106: // SH3ADD.UW
				add_code(dst + " = " + from_reg(instr.Rtype.rs2) + " + (" + src1 + " << 3);");
				break;
			default:
				ILLEGAL_AND_EXIT();
			}
			} break;
		case RV32F_LOAD: {
			const rv32f_instruction fi{instr};
			switch (fi.Itype.funct3) {
			case 0x2: // FLW
				code += "load_fl(&" + from_fpreg(fi.Itype.rd) + ", " + this->memory_load<uint32_t>("uint32_t", fi.Itype.rs1, fi.Itype.signed_imm()) + ");\n";
				break;
			case 0x3: // FLD
				code += "load_dbl(&" + from_fpreg(fi.Itype.rd) + ", " + this->memory_load<uint64_t>("uint64_t", fi.Itype.rs1, fi.Itype.signed_imm()) + ");\n";
				break;
#ifdef RISCV_EXT_VECTOR
			case 0x6: { // VLE32
				const rv32v_instruction vi { instr };
				code += "api.vec_load(cpu, " + std::to_string(vi.VLS.vd) + ", " + from_reg(vi.VLS.rs1) + ");\n";
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
				code += "api.vec_store(cpu, " + from_reg(vi.VLS.rs1) + ", " + std::to_string(vi.VLS.vd) + ");\n";
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
				ILLEGAL_AND_EXIT();
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
				if (UNLIKELY(fi.R4type.rd == 0))
					ILLEGAL_AND_EXIT();
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
					ILLEGAL_AND_EXIT();
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
					ILLEGAL_AND_EXIT();
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
					ILLEGAL_AND_EXIT();
				} break;
			case RV32F__FCVT_SD_DS:
				if (fi.R4type.funct2 == 0x0) {
					code += "set_fl(&" + dst + ", " + rs1 + ".f64);\n";
				} else if (fi.R4type.funct2 == 0x1) {
					code += "set_dbl(&" + dst + ", " + rs1 + ".f32[0]);\n";
				} else {
					ILLEGAL_AND_EXIT();
				} break;
			case RV32F__FCVT_SD_W: {
				const std::string sign((fi.R4type.rs2 == 0x0) ? "(saddr_t)" : "");
				if (fi.R4type.funct2 == 0x0) {
					code += "set_fl(&" + dst + ", " + sign + from_reg(fi.R4type.rs1) + ");\n";
				} else if (fi.R4type.funct2 == 0x1) {
					code += "set_dbl(&" + dst + ", " + sign + from_reg(fi.R4type.rs1) + ");\n";
				} else {
					ILLEGAL_AND_EXIT();
				}
				} break;
			case RV32F__FCVT_W_SD: {
				const std::string sign((fi.R4type.rs2 == 0x0) ? "(int32_t)" : "(uint32_t)");
				if (fi.R4type.rd != 0 && fi.R4type.funct2 == 0x0) {
					code += to_reg(fi.R4type.rd) + " = " + sign + rs1 + ".f32[0];\n";
				} else if (fi.R4type.rd != 0 && fi.R4type.funct2 == 0x1) {
					code += to_reg(fi.R4type.rd) + " = " + sign + rs1 + ".f64;\n";
				} else {
					ILLEGAL_AND_EXIT();
				}
				} break;
			case RV32F__FMV_W_X:
				if (fi.R4type.funct2 == 0x0) {
					code += "load_fl(&" + dst + ", " + from_reg(fi.R4type.rs1) + ");\n";
				} else if (W == 8 && fi.R4type.funct2 == 0x1) {
					code += "load_dbl(&" + dst + ", " + from_reg(fi.R4type.rs1) + ");\n";
				} else {
					ILLEGAL_AND_EXIT();
				} break;
			case RV32F__FMV_X_W:
				if (fi.R4type.funct3 == 0x0) {
					if (fi.R4type.rd != 0 && fi.R4type.funct2 == 0x0) {
						code += to_reg(fi.R4type.rd) + " = " + rs1 + ".i32[0];\n";
					} else if (W == 8 && fi.R4type.rd != 0 && fi.R4type.funct2 == 0x1) { // 64-bit only
						code += to_reg(fi.R4type.rd) + " = " + rs1 + ".i64;\n";
					} else {
						ILLEGAL_AND_EXIT();
					}
				} else { // FPCLASSIFY etc.
					code += "api.execute(cpu, " + std::to_string(instr.whole) + ");\n";
				} break;
			} // fpfunc
			} else ILLEGAL_AND_EXIT();
			} break; // RV32F_FPFUNC
		case RV32A_ATOMIC: // General handler for atomics
			[[fallthrough]];
		case RV32V_OP:	   // General handler for vector instructions
			code += "api.execute(cpu, " + std::to_string(instr.whole) + ");\n";
			break;
		default:
			//throw MachineException(ILLEGAL_OPCODE, "Unhandled instruction in code emitter", instr.opcode());
			ILLEGAL_AND_EXIT();
		}
	}
	// If the function ends with an unimplemented instruction,
	// we must gracefully finish, setting new PC and incrementing IC
	code += "cpu->pc += " + std::to_string((tinfo.len-1) * 4) + ";\n"
			"*cur_insn = c + " + std::to_string(tinfo.len-1) + ";\n";
	exit_function(true);
}

template <int W>
void CPU<W>::emit(std::string& code, const std::string& func, TransInstr<W>* ip, const TransInfo<W>& tinfo) const
{
	Emitter<W> e(const_cast<CPU<W>&>(*this), func, ip, tinfo);
	e.emit();

	// Function header
	code += "extern void " + func + "(CPU* cpu) {\n"
		"uint64_t c = *cur_insn, local_max_insn = *max_insn;\n";

	// Function GPRs
	for (size_t reg = 1; reg < 32; reg++) {
		if (e.get_gpr_exists()[reg]) {
			code += "addr_t " + e.loaded_regname(reg) + " = cpu->r[" + std::to_string(reg) + "];\n";
		}
	}

	// Function code
	code += e.get_code();
}

template void CPU<4>::emit(std::string&, const std::string&, TransInstr<4>*, const TransInfo<4>&) const;
template void CPU<8>::emit(std::string&, const std::string&, TransInstr<8>*, const TransInfo<8>&) const;
} // riscv
