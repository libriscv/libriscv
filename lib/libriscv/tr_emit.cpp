#include "machine.hpp"
#include "instruction_list.hpp"
#include "rv32i_instr.hpp"
#include "rvfd.hpp"
#include <set>

#define PCRELA(x) ((address_t) (tinfo.basepc + i * 4 + (x)))
#define PCRELS(x) std::to_string(PCRELA(x)) + "UL"
#define INSTRUCTION_COUNT(i) ((tinfo.has_branch ? "c + " : "") + std::to_string(i))
#define ILLEGAL_AND_EXIT() { code += "api.exception(cpu, ILLEGAL_OPCODE);\n}\n"; return; }

namespace riscv {
static constexpr int LOOP_INSTRUCTIONS_MAX = 4096;

template <typename ... Args>
inline void add_code(std::string& code, Args&& ... addendum) {
	([&] {
		code += std::string(addendum) + "\n";
	}(), ...);
}
template <int W>
inline std::string from_reg(const TransInfo<W>& tinfo, int reg) {
	if (reg == 3 && tinfo.gp != 0)
		return std::to_string(tinfo.gp);
	else if (reg != 0)
		return "cpu->r[" + std::to_string(reg) + "]";
	return "(addr_t)0";
}
inline std::string from_reg(int reg) {
	if (reg != 0)
		return "cpu->r[" + std::to_string(reg) + "]";
	return "(addr_t)0";
}
inline std::string from_fpreg(int reg) {
	return "cpu->fr[" + std::to_string(reg) + "]";
}
inline std::string from_imm(int64_t imm) {
	return std::to_string(imm);
}
struct BranchInfo {
	bool sign;
	bool goto_enabled;
	int forw_addr;
};
#define FUNCLABEL(i)  (func + "_" + std::to_string(i))
template <int W>
inline void add_branch(std::string& code, const BranchInfo& binfo, const std::string& op, const TransInfo<W>& tinfo, size_t i, rv32i_instruction instr, const std::string& func)
{
	using address_t = address_type<W>;
	if (binfo.sign == false)
		code += "if (" + from_reg(tinfo, instr.Btype.rs1) + op + from_reg(tinfo, instr.Btype.rs2) + ") {\n";
	else
		code += "if ((saddr_t)" + from_reg(tinfo, instr.Btype.rs1) + op + " (saddr_t)" + from_reg(tinfo, instr.Btype.rs2) + ") {\n";
	if (binfo.goto_enabled) {
		// this is a jump back to the start of the function
		code += "c += " + std::to_string(i) + "; if (c < " + std::to_string(LOOP_INSTRUCTIONS_MAX) + ") goto " + func + "_start;\n";
		// We can simplify this jump because we know it's safe
		// This side of the branch exits bintr because of max instructions reached
		code += "cpu->pc = " + PCRELS(instr.Btype.signed_imm() - 4) + ";\n"
			"return;}\n";
	} else if (binfo.forw_addr > 0) {
		code += "goto " + FUNCLABEL(binfo.forw_addr) + ";\n"
				"}\n";
	} else {
	// The number of instructions to increment depends on if branch-instruction-counting is enabled
	code += "api.jump(cpu, " + PCRELS(instr.Btype.signed_imm() - 4) + ", " + (tinfo.has_branch ? "c" : std::to_string(i)) + ");\n"
		"return;}\n";
	}
}
template <int W>
inline void emit_op(std::string& code, const std::string& op, const std::string& sop,
	const TransInfo<W>& tinfo, uint32_t rd, uint32_t rs1, const std::string& rs2)
{
	if (rd == 0) {
		/* must be a NOP */
	} else if (rd == rs1) {
		add_code(code, from_reg(rd) + sop + rs2 + ";");
	} else {
	add_code(code,
		from_reg(rd) + " = " + from_reg(tinfo, rs1) + op + rs2 + ";");
	}
}

template <int W>
void CPU<W>::emit(std::string& code, const std::string& func, TransInstr<W>* ip, const TransInfo<W>& tinfo) const
{
	static constexpr unsigned XLEN = W * 8u;
	static const std::string SIGNEXTW = "(saddr_t) (int32_t)";
	std::set<unsigned> labels;
	code += "extern void " + func + "(CPU* cpu) {\n";
	// branches can jump back, within limits
	if (tinfo.has_branch) {
		code += "int c = 0; " + func + "_start:;\n";
	}

	auto current_pc = tinfo.basepc;

	for (int i = 0; i < tinfo.len; i++) {
		const auto instr = rv32i_instruction {ip[i].instr};

		// known jump locations
		if (tinfo.jump_locations.count(current_pc) > 0) {
			code.append(FUNCLABEL(i) + ":;\n");
		}
		else if (labels.count(i) > 0)
		{ // forward branches (empty statement)
			code.append(FUNCLABEL(i) + ":;\n");
		}
		// instruction generation
		switch (instr.opcode()) {
		case RV32I_LOAD:
			switch (instr.Itype.funct3) {
			case 0x0: // I8
				if (instr.Itype.rd == 0) {
					add_code(code,
					"api.mem_ld8(cpu, " + from_reg(tinfo, instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");");
				} else {
					add_code(code,
					from_reg(instr.Itype.rd) + " = (saddr_t)(int8_t)api.mem_ld8(cpu, " + from_reg(tinfo, instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");");
				} break;
			case 0x1: // I16
				if (instr.Itype.rd == 0) {
					add_code(code,
					"api.mem_ld16(cpu, " + from_reg(tinfo, instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");");
				} else {
					add_code(code,
					from_reg(instr.Itype.rd) + " = (saddr_t)(int16_t)api.mem_ld16(cpu, " + from_reg(tinfo, instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");");
				} break;
			case 0x2: // I32
				if (instr.Itype.rd == 0) {
					add_code(code,
					"api.mem_ld32(cpu, " + from_reg(tinfo, instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");");
				} else {
					if constexpr (W == 4) {
						add_code(code,
							from_reg(instr.Itype.rd) + " = api.mem_ld32(cpu, " + from_reg(tinfo, instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");");
					} else {
						add_code(code,
							from_reg(instr.Itype.rd) + " = (saddr_t)(int32_t)api.mem_ld32(cpu, " + from_reg(tinfo, instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");");
					}
				} break;
			case 0x3: // I64
				if (instr.Itype.rd == 0) {
					add_code(code,
					"api.mem_ld64(cpu, " + from_reg(tinfo, instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");");
				} else {
					add_code(code,
					from_reg(instr.Itype.rd) + " = api.mem_ld64(cpu, " + from_reg(tinfo, instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");");
				}
				break;
			case 0x4: // U8
				add_code(code,
				from_reg(instr.Itype.rd) + " = api.mem_ld8(cpu, " + from_reg(tinfo, instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");");
				break;
			case 0x5: // U16
				add_code(code,
				from_reg(instr.Itype.rd) + " = api.mem_ld16(cpu, " + from_reg(tinfo, instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");");
				break;
			case 0x6: // U32
				add_code(code,
				from_reg(instr.Itype.rd) + " = api.mem_ld32(cpu, " + from_reg(tinfo, instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");");
				break;
			default:
				ILLEGAL_AND_EXIT();
			} break;
		case RV32I_STORE:
			switch (instr.Stype.funct3) {
			case 0x0: // I8
				add_code(code,
					"api.mem_st8(cpu, " + from_reg(tinfo, instr.Stype.rs1) + " + " + from_imm(instr.Stype.signed_imm()) + ", " + from_reg(tinfo, instr.Stype.rs2) + ");");
				break;
			case 0x1: // I16
				add_code(code,
					"api.mem_st16(cpu, " + from_reg(tinfo, instr.Stype.rs1) + " + " + from_imm(instr.Stype.signed_imm()) + ", " + from_reg(tinfo, instr.Stype.rs2) + ");");
				break;
			case 0x2: // I32
				add_code(code,
					"api.mem_st32(cpu, " + from_reg(tinfo, instr.Stype.rs1) + " + " + from_imm(instr.Stype.signed_imm()) + ", " + from_reg(tinfo, instr.Stype.rs2) + ");");
				break;
			case 0x3: // I64
				add_code(code,
					"api.mem_st64(cpu, " + from_reg(tinfo, instr.Stype.rs1) + " + " + from_imm(instr.Stype.signed_imm()) + ", " + from_reg(tinfo, instr.Stype.rs2) + ");");
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
				fl = i+offset;
				labels.insert(fl);
			} else if (tinfo.jump_locations.count(PCRELA(offset * 4))) {
				const int dstidx = i + offset;
				if (dstidx >= 0 && dstidx < tinfo.len) {
					fl = i+offset;
				}
			}
			switch (instr.Btype.funct3) {
			case 0x0: // EQ
				add_branch<W>(code, { false, ge, fl }, " == ", tinfo, i, instr, func);
				break;
			case 0x1: // NE
				add_branch<W>(code, { false, ge, fl }, " != ", tinfo, i, instr, func);
				break;
			case 0x2:
			case 0x3:
				ILLEGAL_AND_EXIT();
			case 0x4: // LT
				add_branch<W>(code, { true, ge, fl }, " < ", tinfo, i, instr, func);
				break;
			case 0x5: // GE
				add_branch<W>(code, { true, ge, fl }, " >= ", tinfo, i, instr, func);
				break;
			case 0x6: // LTU
				add_branch<W>(code, { false, ge, fl }, " < ", tinfo, i, instr, func);
				break;
			case 0x7: // GEU
				add_branch<W>(code, { false, ge, fl }, " >= ", tinfo, i, instr, func);
				break;
			} } break;
		case RV32I_JALR: {
			// jump to register + immediate
			// NOTE: We need to remember RS1 because it can be clobbered by RD
			add_code(code, "addr_t jrs1 = " + from_reg(tinfo, instr.Itype.rs1) + ";");
			if (instr.Itype.rd != 0) {
				add_code(code, from_reg(instr.Itype.rd) + " = " + PCRELS(4) + ";");
			}
			add_code(code, "api.jump(cpu, jrs1 + "
				+ from_imm(instr.Itype.signed_imm()) + " - 4, " + INSTRUCTION_COUNT(i) + ");",
				"}");
			} return;
		case RV32I_JAL: {
			if (instr.Jtype.rd != 0) {
				add_code(code, from_reg(instr.Jtype.rd) + " = " + PCRELS(4) + ";\n");
			}
			// forward label: jump inside code block
			const auto offset = instr.Jtype.jump_offset() / 4;
			int fl = i+offset;
			if (fl > 0 && fl < tinfo.len) {
				// forward labels require creating future labels
				if (offset > 0)
					labels.insert(fl);
				// this is a jump back to the start of the function
				add_code(code, "c += " + std::to_string(i) + "; if (c < " + std::to_string(LOOP_INSTRUCTIONS_MAX) + ") goto " + FUNCLABEL(fl) + ";");
				// if we run out of instructions, we must exit:
				// XXX: instruction counting, what a mess
				add_code(code, "cpu->pc = " + PCRELS(instr.Jtype.jump_offset() - 4) + ";\n"
					"return;");
				break;
			} else {
				// Because of forward jumps we can't end the function here
				add_code(code,
					"api.jump(cpu, " + PCRELS(instr.Jtype.jump_offset() - 4) + ", " + INSTRUCTION_COUNT(i) + ");",
					"return;");
				break;
			} };
		case RV32I_OP_IMM: {
			// NOP
			if (UNLIKELY(instr.Itype.rd == 0)) break;
			const auto dst = from_reg(instr.Itype.rd);
			const auto src = from_reg(tinfo, instr.Itype.rs1);
			switch (instr.Itype.funct3) {
			case 0x0: // ADDI
				if (instr.Itype.signed_imm() == 0) {
					add_code(code, dst + " = " + src + ";");
				} else {
					emit_op(code, " + ", " += ", tinfo, instr.Itype.rd, instr.Itype.rs1, from_imm(instr.Itype.signed_imm()));
				} break;
			case 0x1: // SLLI
				// SLLI: Logical left-shift 5/6-bit immediate
				emit_op(code, " << ", " <<= ", tinfo, instr.Itype.rd, instr.Itype.rs1,
					std::to_string(instr.Itype.shift64_imm() & (XLEN-1)));
				break;
			case 0x2: // SLTI:
				// signed less than immediate
				add_code(code,
					dst + " = ((saddr_t)" + src + " < " + from_imm(instr.Itype.signed_imm()) + ") ? 1 : 0;");
				break;
			case 0x3: // SLTU:
				add_code(code,
					dst + " = (" + src + " < (unsigned) " + from_imm(instr.Itype.signed_imm()) + ") ? 1 : 0;");
				break;
			case 0x4: // XORI:
				emit_op(code, " ^ ", " ^= ", tinfo, instr.Itype.rd, instr.Itype.rs1, from_imm(instr.Itype.signed_imm()));
				break;
			case 0x5: // SRLI / SRAI:
				if (LIKELY(!instr.Itype.is_srai())) {
					emit_op(code, " >> ", " >>= ", tinfo, instr.Itype.rd, instr.Itype.rs1,
						std::to_string(instr.Itype.shift64_imm() & (XLEN-1)));
				} else { // SRAI: preserve the sign bit
					add_code(code,
						dst + " = (saddr_t)" + src + " >> (" + from_imm(instr.Itype.signed_imm()) + " & (XLEN-1));");
				}
				break;
			case 0x6: // ORI
				add_code(code,
					dst + " = " + src + " | " + from_imm(instr.Itype.signed_imm()) + ";");
				break;
			case 0x7: // ANDI
				add_code(code,
					dst + " = " + src + " & " + from_imm(instr.Itype.signed_imm()) + ";");
				break;
			}
			} break;
		case RV32I_OP:
			if (UNLIKELY(instr.Rtype.rd == 0)) break;

			switch (instr.Rtype.jumptable_friendly_op()) {
			case 0x0: // ADD
				emit_op(code, " + ", " += ", tinfo, instr.Rtype.rd, instr.Rtype.rs1, from_reg(instr.Rtype.rs2));
				break;
			case 0x200: // SUB
				emit_op(code, " - ", " -= ", tinfo, instr.Rtype.rd, instr.Rtype.rs1, from_reg(instr.Rtype.rs2));
				break;
			case 0x1: // SLL
				add_code(code,
					from_reg(instr.Rtype.rd) + " = " + from_reg(tinfo, instr.Rtype.rs1) + " << (" + from_reg(tinfo, instr.Rtype.rs2) + " & (XLEN-1));");
				break;
			case 0x2: // SLT
				add_code(code,
					from_reg(instr.Rtype.rd) + " = ((saddr_t)" + from_reg(tinfo, instr.Rtype.rs1) + " < (saddr_t)" + from_reg(tinfo, instr.Rtype.rs2) + ") ? 1 : 0;");
				break;
			case 0x3: // SLTU
				add_code(code,
					from_reg(instr.Rtype.rd) + " = (" + from_reg(tinfo, instr.Rtype.rs1) + " < " + from_reg(tinfo, instr.Rtype.rs2) + ") ? 1 : 0;");
				break;
			case 0x4: // XOR
				emit_op(code, " ^ ", " ^= ", tinfo, instr.Rtype.rd, instr.Rtype.rs1, from_reg(instr.Rtype.rs2));
				break;
			case 0x5: // SRL
				add_code(code,
					from_reg(instr.Rtype.rd) + " = " + from_reg(tinfo, instr.Rtype.rs1) + " >> (" + from_reg(tinfo, instr.Rtype.rs2) + " & (XLEN-1));");
				break;
			case 0x205: // SRA
				add_code(code,
					from_reg(instr.Rtype.rd) + " = (saddr_t)" + from_reg(tinfo, instr.Rtype.rs1) + " >> (" + from_reg(tinfo, instr.Rtype.rs2) + " & (XLEN-1));");
				break;
			case 0x6: // OR
				emit_op(code, " | ", " |= ", tinfo, instr.Rtype.rd, instr.Rtype.rs1, from_reg(instr.Rtype.rs2));
				break;
			case 0x7: // AND
				emit_op(code, " & ", " &= ", tinfo, instr.Rtype.rd, instr.Rtype.rs1, from_reg(instr.Rtype.rs2));
				break;
			// extension RV32M / RV64M
			case 0x10: // MUL
				add_code(code,
					from_reg(instr.Rtype.rd) + " = (saddr_t)" + from_reg(tinfo, instr.Rtype.rs1) + " * (saddr_t)" + from_reg(tinfo, instr.Rtype.rs2) + ";");
				break;
			case 0x11: // MULH (signed x signed)
				add_code(code,
					(W == 4) ?
					from_reg(instr.Rtype.rd) + " = (uint64_t)((int64_t)(saddr_t)" + from_reg(tinfo, instr.Rtype.rs1) + " * (int64_t)(saddr_t)" + from_reg(tinfo, instr.Rtype.rs2) + ") >> 32u;" :
					"MUL128(&" + from_reg(tinfo, instr.Rtype.rd) + ", " + from_reg(tinfo, instr.Rtype.rs1) + ", " + from_reg(tinfo, instr.Rtype.rs2) + ");"
				);
				break;
			case 0x12: // MULHSU (signed x unsigned)
				add_code(code,
					(W == 4) ?
					from_reg(instr.Rtype.rd) + " = (uint64_t)((int64_t)(saddr_t)" + from_reg(tinfo, instr.Rtype.rs1) + " * (uint64_t)" + from_reg(tinfo, instr.Rtype.rs2) + ") >> 32u;" :
					"MUL128(&" + from_reg(tinfo, instr.Rtype.rd) + ", " + from_reg(tinfo, instr.Rtype.rs1) + ", " + from_reg(tinfo, instr.Rtype.rs2) + ");"
				);
				break;
			case 0x13: // MULHU (unsigned x unsigned)
				add_code(code,
					(W == 4) ?
					from_reg(instr.Rtype.rd) + " = ((uint64_t) " + from_reg(tinfo, instr.Rtype.rs1) + " * (uint64_t)" + from_reg(tinfo, instr.Rtype.rs2) + ") >> 32u;" :
					"MUL128(&" + from_reg(tinfo, instr.Rtype.rd) + ", " + from_reg(tinfo, instr.Rtype.rs1) + ", " + from_reg(tinfo, instr.Rtype.rs2) + ");"
				);
				break;
			case 0x14: // DIV
				// division by zero is not an exception
				if constexpr (W == 8) {
					add_code(code,
						"if (LIKELY(" + from_reg(tinfo, instr.Rtype.rs2) + " != 0)) {",
						"	if (LIKELY(!(" + from_reg(tinfo, instr.Rtype.rs1) + " == -9223372036854775808ull && " + from_reg(tinfo, instr.Rtype.rs2) + " == -1ull)))"
						"		" + from_reg(tinfo, instr.Rtype.rd) + " = (int64_t)" + from_reg(tinfo, instr.Rtype.rs1) + " / (int64_t)" + from_reg(tinfo, instr.Rtype.rs2) + ";",
						"}");
				} else {
					add_code(code,
						"if (LIKELY(" + from_reg(tinfo, instr.Rtype.rs2) + " != 0)) {",
						"	if (LIKELY(!(" + from_reg(tinfo, instr.Rtype.rs1) + " == 2147483648 && " + from_reg(tinfo, instr.Rtype.rs2) + " == 4294967295)))",
						"		" + from_reg(tinfo, instr.Rtype.rd) + " = (int32_t)" + from_reg(tinfo, instr.Rtype.rs1) + " / (int32_t)" + from_reg(tinfo, instr.Rtype.rs2) + ";",
						"}");
				}
				break;
			case 0x15: // DIVU
				add_code(code,
					"if (LIKELY(" + from_reg(tinfo, instr.Rtype.rs2) + " != 0))",
					from_reg(instr.Rtype.rd) + " = " + from_reg(tinfo, instr.Rtype.rs1) + " / " + from_reg(tinfo, instr.Rtype.rs2) + ";"
				);
				break;
			case 0x16: // REM
				if constexpr (W == 8) {
					add_code(code,
					"if (LIKELY(" + from_reg(tinfo, instr.Rtype.rs2) + " != 0)) {",
					"	if (LIKELY(!(" + from_reg(tinfo, instr.Rtype.rs1) + " == -9223372036854775808ull && " + from_reg(tinfo, instr.Rtype.rs2) + " == -1ull)))",
					"		" + from_reg(tinfo, instr.Rtype.rd) + " = (int64_t)" + from_reg(tinfo, instr.Rtype.rs1) + " % (int64_t)" + from_reg(tinfo, instr.Rtype.rs2) + ";",
					"}");
				} else {
					add_code(code,
					"if (LIKELY(" + from_reg(tinfo, instr.Rtype.rs2) + " != 0)) {",
					"	if (LIKELY(!(" + from_reg(tinfo, instr.Rtype.rs1) + " == 2147483648 && " + from_reg(tinfo, instr.Rtype.rs2) + " == 4294967295)))",
					"		" + from_reg(tinfo, instr.Rtype.rd) + " = (int32_t)" + from_reg(tinfo, instr.Rtype.rs1) + " % (int32_t)" + from_reg(tinfo, instr.Rtype.rs2) + ";",
					"}");
				}
				break;
			case 0x17: // REMU
				add_code(code,
				"if (LIKELY(" + from_reg(tinfo, instr.Rtype.rs2) + " != 0))",
					from_reg(instr.Rtype.rd) + " = " + from_reg(tinfo, instr.Rtype.rs1) + " % " + from_reg(tinfo, instr.Rtype.rs2) + ";"
				);
				break;
			case 0x102: // SH1ADD
				add_code(code, from_reg(instr.Rtype.rd) + " = " + from_reg(instr.Rtype.rs2) + " + (" + from_reg(instr.Rtype.rs1) + " << 1);");
				break;
			case 0x104: // SH2ADD
				add_code(code, from_reg(instr.Rtype.rd) + " = " + from_reg(instr.Rtype.rs2) + " + (" + from_reg(instr.Rtype.rs1) + " << 2);");
				break;
			case 0x106: // SH3ADD
				add_code(code, from_reg(instr.Rtype.rd) + " = " + from_reg(instr.Rtype.rs2) + " + (" + from_reg(instr.Rtype.rs1) + " << 3);");
				break;
			case 0x204: // XNOR
				add_code(code, from_reg(instr.Rtype.rd) + " = ~(" + from_reg(instr.Rtype.rs1) + " ^ " + from_reg(instr.Rtype.rs2) + " << 2);");
				break;
			default:
				ILLEGAL_AND_EXIT();
				//fprintf(stderr, "RV32I_OP: Unhandled function 0x%X\n",
				//		instr.Rtype.jumptable_friendly_op());
			}
			break;
		case RV32I_LUI:
			if (UNLIKELY(instr.Utype.rd == 0))
				ILLEGAL_AND_EXIT();
			add_code(code,
				from_reg(instr.Utype.rd) + " = " + from_imm(instr.Utype.upper_imm()) + ";");
			break;
		case RV32I_AUIPC:
			if (UNLIKELY(instr.Utype.rd == 0))
				ILLEGAL_AND_EXIT();
			add_code(code,
				from_reg(instr.Utype.rd) + " = " + PCRELS(instr.Utype.upper_imm()) + ";");
			break;
		case RV32I_FENCE:
			break;
		case RV32I_SYSTEM:
			if (instr.Itype.funct3 == 0x0) {
				if (instr.Itype.imm == 0) {
					code += "if (UNLIKELY(api.syscall(cpu, " + from_reg(17) + ", " + INSTRUCTION_COUNT(i) + ")))\n"
					       "  return;\n";
					break;
				} if (instr.Itype.imm == 1) {
					code += "api.ebreak(cpu, " + INSTRUCTION_COUNT(i) + ");\n}\n";
					return; // !!
				} if (instr.Itype.imm == 261) {
					code += "api.stop(cpu, " + INSTRUCTION_COUNT(i) + ");\n}\n";
					return; // !!
				} else {
					code += "api.system(cpu, " + std::to_string(instr.whole) +");\n";
					break;
				}
			} else {
				code += "api.system(cpu, " + std::to_string(instr.whole) +");\n";
			} break;
		case RV64I_OP_IMM32: {
			if (UNLIKELY(instr.Itype.rd == 0))
				ILLEGAL_AND_EXIT();
			const auto dst = from_reg(instr.Itype.rd);
			const auto src = "(uint32_t)" + from_reg(tinfo, instr.Itype.rs1);
			switch (instr.Itype.funct3) {
			case 0x0:
				// ADDIW: Add sign-extended 12-bit immediate
				add_code(code, dst + " = " + SIGNEXTW + " (" + src + " + " + from_imm(instr.Itype.signed_imm()) + ");");
				break;
			case 0x1: // SLLIW:
				add_code(code, dst + " = " + SIGNEXTW + " (" + src + " << " + from_imm(instr.Itype.shift_imm()) + ");");
				break;
			case 0x5: // SRLIW / SRAIW:
				if (LIKELY(!instr.Itype.is_srai())) {
					add_code(code, dst + " = " + SIGNEXTW + " (" + src + " >> " + from_imm(instr.Itype.shift_imm()) + ");");
				} else { // SRAIW: preserve the sign bit
					add_code(code,
						dst + " = (int32_t)" + src + " >> " + from_imm(instr.Itype.shift_imm()) + ";");
				}
				break;
			default:
				ILLEGAL_AND_EXIT();
			}
			} break;
		case RV64I_OP32: {
			if (UNLIKELY(instr.Rtype.rd == 0))
				ILLEGAL_AND_EXIT();
			const auto dst = from_reg(instr.Rtype.rd);
			const auto src1 = "(uint32_t)" + from_reg(tinfo, instr.Rtype.rs1);
			const auto src2 = "(uint32_t)" + from_reg(tinfo, instr.Rtype.rs2);

			switch (instr.Rtype.jumptable_friendly_op()) {
			case 0x0: // ADDW
				add_code(code, dst + " = " + SIGNEXTW + " (" + src1 + " + " + src2 + ");");
				break;
			case 0x200: // SUBW
				add_code(code, dst + " = " + SIGNEXTW + " (" + src1 + " - " + src2 + ");");
				break;
			case 0x1: // SLLW
				add_code(code, dst + " = " + SIGNEXTW + " (" + src1 + " << (" + src2 + " & 0x1F));");
				break;
			case 0x5: // SRLW
				add_code(code, dst + " = " + SIGNEXTW + " (" + src1 + " >> (" + src2 + " & 0x1F));");
				break;
			case 0x205: // SRAW
				add_code(code, dst + " = (int32_t)" + src1 + " >> (" + src2 + " & 31);");
				break;
			// M-extension
			case 0x10: // MULW
				add_code(code, dst + " = " + SIGNEXTW + "(" + src1 + " * " + src2 + ");");
				break;
			case 0x14: // DIVW
				// division by zero is not an exception
				add_code(code,
				"if (LIKELY(" + src2 + " != 0))",
				"if (LIKELY(!((int32_t)" + src1 + " == -2147483648 && (int32_t)" + src2 + " == -1)))",
				dst + " = " + SIGNEXTW + " ((int32_t)" + src1 + " / (int32_t)" + src2 + ");");
				break;
			case 0x15: // DIVUW
				add_code(code,
				"if (LIKELY(" + src2 + " != 0))",
				dst + " = " + SIGNEXTW + " (" + src1 + " / " + src2 + ");");
				break;
			case 0x16: // REMW
				add_code(code,
				"if (LIKELY(" + src2 + " != 0))",
				"if (LIKELY(!((int32_t)" + src1 + " == -2147483648 && (int32_t)" + src2 + " == -1)))",
				dst + " = " + SIGNEXTW + " ((int32_t)" + src1 + " % (int32_t)" + src2 + ");");
				break;
			case 0x17: // REMUW
				add_code(code,
				"if (LIKELY(" + src2 + " != 0))",
				dst + " = " + SIGNEXTW + " (" + src1 + " % " + src2 + ");");
				break;
			case 0x40: // ADDUW
				add_code(code, dst + " = " + from_reg(tinfo, instr.Rtype.rs2) + " + " + src1 + ";");
				break;
			case 0x102: // SH1ADD.UW
				add_code(code, dst + " = " + from_reg(tinfo, instr.Rtype.rs2) + " + (" + src1 + " << 1);");
				break;
			case 0x104: // SH2ADD.UW
				add_code(code, dst + " = " + from_reg(tinfo, instr.Rtype.rs2) + " + (" + src1 + " << 2);");
				break;
			case 0x106: // SH3ADD.UW
				add_code(code, dst + " = " + from_reg(tinfo, instr.Rtype.rs2) + " + (" + src1 + " << 3);");
				break;
			default:
				ILLEGAL_AND_EXIT();
			}
			} break;
		case RV32F_LOAD: {
			const rv32f_instruction fi{instr};
			const auto addr = from_reg(tinfo, fi.Itype.rs1) + " + " + from_imm(fi.Itype.signed_imm());
			switch (fi.Itype.funct3) {
			case 0x2: // FLW
				code += "load_fl(&" + from_fpreg(fi.Itype.rd) + ", api.mem_ld32(cpu, " + addr + "));\n";
				break;
			case 0x3: // FLD
				code += "load_dbl(&" + from_fpreg(fi.Itype.rd) + ", api.mem_ld64(cpu, " + addr + "));\n";
				break;
			default:
				code += "api.execute(cpu, " + std::to_string(instr.whole) + ");\n";
				break;
			}
			} break;
		case RV32F_STORE: {
			const rv32f_instruction fi{instr};
			const auto addr = from_reg(tinfo, fi.Stype.rs1) + " + " + from_imm(fi.Stype.signed_imm());
			switch (fi.Itype.funct3) {
			case 0x2: // FSW
				code += "api.mem_st32(cpu, " + addr + ", " + from_fpreg(fi.Stype.rs2) + ".i32[0]);\n";
				break;
			case 0x3: // FSD
				code += "api.mem_st64(cpu, " + addr + ", " + from_fpreg(fi.Stype.rs2) + ".i64);\n";
				break;
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
					code += from_reg(fi.R4type.rd) + " = (" + rs1 + ".f32[0] <= " + rs2 + ".f32[0]) ? 1 : 0;\n";
					break;
				case 0x1: // FLT.S
					code += from_reg(fi.R4type.rd) + " = (" + rs1 + ".f32[0] < " + rs2 + ".f32[0]) ? 1 : 0;\n";
					break;
				case 0x2: // FEQ.S
					code += from_reg(fi.R4type.rd) + " = (" + rs1 + ".f32[0] == " + rs2 + ".f32[0]) ? 1 : 0;\n";
					break;
				case 0x10: // FLE.D
					code += from_reg(fi.R4type.rd) + " = (" + rs1 + ".f64 <= " + rs2 + ".f64) ? 1 : 0;\n";
					break;
				case 0x11: // FLT.D
					code += from_reg(fi.R4type.rd) + " = (" + rs1 + ".f64 < " + rs2 + ".f64) ? 1 : 0;\n";
					break;
				case 0x12: // FEQ.D
					code += from_reg(fi.R4type.rd) + " = (" + rs1 + ".f64 == " + rs2 + ".f64) ? 1 : 0;\n";
					break;
				default:
					ILLEGAL_AND_EXIT();
				} break;
			case RV32F__FMIN_MAX:
				switch (fi.R4type.funct3 | (fi.R4type.funct2 << 4)) {
				case 0x0: // FMIN.S
					code += "set_fl(&" + dst + ", __builtin_fminf(" + rs1 + ".f32[0], " + rs2 + ".f32[0]));\n";
					break;
				case 0x1: // FMAX.S
					code += "set_fl(&" + dst + ", __builtin_fmaxf(" + rs1 + ".f32[0], " + rs2 + ".f32[0]));\n";
					break;
				case 0x10: // FMIN.D
					code += "set_dbl(&" + dst + ", __builtin_fmin(" + rs1 + ".f64, " + rs2 + ".f64));\n";
					break;
				case 0x11: // FMAX.D
					code += "set_dbl(&" + dst + ", __builtin_fmax(" + rs1 + ".f64, " + rs2 + ".f64));\n";
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
					code += "set_fl(&" + dst + ", " + sign + from_reg(tinfo, fi.R4type.rs1) + ");\n";
				} else if (fi.R4type.funct2 == 0x1) {
					code += "set_dbl(&" + dst + ", " + sign + from_reg(tinfo, fi.R4type.rs1) + ");\n";
				} else {
					ILLEGAL_AND_EXIT();
				}
				} break;
			case RV32F__FCVT_W_SD: {
				const std::string sign((fi.R4type.rs2 == 0x0) ? "(int32_t)" : "(uint32_t)");
				if (fi.R4type.rd != 0 && fi.R4type.funct2 == 0x0) {
					code += from_reg(fi.R4type.rd) + " = " + sign + rs1 + ".f32[0];\n";
				} else if (fi.R4type.rd != 0 && fi.R4type.funct2 == 0x1) {
					code += from_reg(fi.R4type.rd) + " = " + sign + rs1 + ".f64;\n";
				} else {
					ILLEGAL_AND_EXIT();
				}
				} break;
			case RV32F__FMV_W_X:
				if (fi.R4type.funct2 == 0x0) {
					code += "load_fl(&" + dst + ", " + from_reg(tinfo, fi.R4type.rs1) + ");\n";
				} else if (W == 8 && fi.R4type.funct2 == 0x1) {
					code += "load_dbl(&" + dst + ", " + from_reg(tinfo, fi.R4type.rs1) + ");\n";
				} else {
					ILLEGAL_AND_EXIT();
				} break;
			case RV32F__FMV_X_W:
				if (fi.R4type.funct3 == 0x0) {
					if (fi.R4type.rd != 0 && fi.R4type.funct2 == 0x0) {
						code += from_reg(fi.R4type.rd) + " = " + rs1 + ".i32[0];\n";
					} else if (W == 8 && fi.R4type.rd != 0 && fi.R4type.funct2 == 0x1) { // 64-bit only
						code += from_reg(fi.R4type.rd) + " = " + rs1 + ".i64;\n";
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

		current_pc += instr.length();
	}
	// If the function ends with an unimplemented instruction,
	// we must gracefully finish, setting new PC and incrementing IC
	code += "api.finish(cpu, " + std::to_string(tinfo.len-1) + ", " + INSTRUCTION_COUNT(tinfo.len-1) + ");\n}\n";
}

template void CPU<4>::emit(std::string&, const std::string&, TransInstr<4>*, const TransInfo<4>&) const;
template void CPU<8>::emit(std::string&, const std::string&, TransInstr<8>*, const TransInfo<8>&) const;
} // riscv
