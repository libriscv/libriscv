#define IS_OPCODE(instr, opcode) (instr.opcode() == opcode)
#define IS_HANDLER(ip, instr) ((ip).first == DECODED_INSTR(instr).handler)
#define IS_FPHANDLER(ip, instr) ((ip).first == DECODED_FLOAT(instr).handler)
#define PCREL(x) std::to_string((address_t) (basepc + i * 4 + (x)))
#define ILLEGAL_AND_EXIT() { code += "api.exception(cpu, ILLEGAL_OPCODE);\n}\n"; return; }

template <typename ... Args>
inline void add_code(std::string& code, Args&& ... addendum) {
	([&] {
		code += std::string(addendum) + "\n";
	}(), ...);
}
inline std::string from_reg(int reg) {
	if (reg != 0)
		return "cpu->r[" + std::to_string(reg) + "]";
	return "0";
}
inline std::string from_fpreg(int reg) {
	return "cpu->fr[" + std::to_string(reg) + "]";
}
inline std::string from_imm(int64_t imm) {
	return std::to_string(imm);
}
template <int W>
inline void add_branch(std::string& code, bool sign, const std::string& op, address_type<W> basepc, size_t i, rv32i_instruction instr)
{
	using address_t = address_type<W>;
	add_code(code,
		((sign == false) ?
		"if (" + from_reg(instr.Btype.rs1) + op + from_reg(instr.Btype.rs2) + ") {" :
		"if ((saddr_t)" + from_reg(instr.Btype.rs1) + op + " (saddr_t)" + from_reg(instr.Btype.rs2) + ") {"),
		"api.jump(cpu, " + PCREL(instr.Btype.signed_imm() - 4) + ", " + std::to_string(i) + ");",
		"return;}");
}
inline void emit_op(std::string& code, const std::string& op, const std::string& sop,
	uint32_t rd, uint32_t rs1, const std::string& rs2)
{
	if (rd == 0) {
		/* must be a NOP */
	} else if (rd == rs1) {
		add_code(code, from_reg(rd) + sop + rs2 + ";");
	} else {
	add_code(code,
		from_reg(rd) + " = " + from_reg(rs1) + op + rs2 + ";");
	}
}

template <int W>
void CPU<W>::emit(std::string& code, const std::string& func, address_t basepc, instr_pair* ip, size_t len) const
{
	static const std::string SIGNEXTW = "(saddr_t) (int32_t)";
	code += "extern void " + func + "(CPU* cpu) {\n";
	for (size_t i = 0; i < len; i++) {
		const auto& instr = ip[i].second;
		switch (instr.opcode()) {
		case RV32I_LOAD:
			if (IS_HANDLER(ip[i], LOAD_I8_DUMMY)) {
				add_code(code,
					"api.mem_ld8(cpu, " + from_reg(instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");"
				);
			}
			else if (IS_HANDLER(ip[i], LOAD_I8)) {
				add_code(code,
					from_reg(instr.Itype.rd) + " = (saddr_t) (int8_t) api.mem_ld8(cpu, " + from_reg(instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");"
				);
			}
			else if (IS_HANDLER(ip[i], LOAD_I16_DUMMY)) {
				add_code(code,
					"api.mem_ld16(cpu, " + from_reg(instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");"
				);
			}
			else if (IS_HANDLER(ip[i], LOAD_I16)) {
				add_code(code,
					from_reg(instr.Itype.rd) + " = (saddr_t) (int16_t) api.mem_ld16(cpu, " + from_reg(instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");"
				);
			}
			else if (IS_HANDLER(ip[i], LOAD_I32_DUMMY)) {
				add_code(code,
					"api.mem_ld32(cpu, " + from_reg(instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");"
				);
			}
			else if (IS_HANDLER(ip[i], LOAD_I32)) {
				add_code(code,
					from_reg(instr.Itype.rd) + " = (saddr_t) (int32_t) api.mem_ld32(cpu, " + from_reg(instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");"
				);
			}
			else if (IS_HANDLER(ip[i], LOAD_U8)) {
				add_code(code,
					from_reg(instr.Itype.rd) + " = api.mem_ld8(cpu, " + from_reg(instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");"
				);
			}
			else if (IS_HANDLER(ip[i], LOAD_U16)) {
				add_code(code,
					from_reg(instr.Itype.rd) + " = api.mem_ld16(cpu, " + from_reg(instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");"
				);
			}
			else if (IS_HANDLER(ip[i], LOAD_U32)) {
				add_code(code,
					from_reg(instr.Itype.rd) + " = api.mem_ld32(cpu, " + from_reg(instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");"
				);
			}
			else if (IS_HANDLER(ip[i], LOAD_U64)) {
				add_code(code,
					from_reg(instr.Itype.rd) + " = api.mem_ld64(cpu, " + from_reg(instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");"
				);
			}
			else if (IS_HANDLER(ip[i], LOAD_U64_DUMMY)) {
				add_code(code,
					"api.mem_ld64(cpu, " + from_reg(instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");"
				);
			} else {
				throw std::runtime_error("Unhandled load opcode");
			}
			break;
		case RV32I_STORE:
			if (IS_HANDLER(ip[i], STORE_I8) || IS_HANDLER(ip[i], STORE_I8_IMM)) {
				add_code(code,
					"api.mem_st8(cpu, " + from_reg(instr.Stype.rs1) + " + " + from_imm(instr.Stype.signed_imm()) + ", " + from_reg(instr.Stype.rs2) + ");"
				);
			}
			else if (IS_HANDLER(ip[i], STORE_I16_IMM)) {
				add_code(code,
					"api.mem_st16(cpu, " + from_reg(instr.Stype.rs1) + " + " + from_imm(instr.Stype.signed_imm()) + ", " + from_reg(instr.Stype.rs2) + ");"
				);
			}
			else if (IS_HANDLER(ip[i], STORE_I32_IMM)) {
				add_code(code,
					"api.mem_st32(cpu, " + from_reg(instr.Stype.rs1) + " + " + from_imm(instr.Stype.signed_imm()) + ", " + from_reg(instr.Stype.rs2) + ");"
				);
			}
			else if (IS_HANDLER(ip[i], STORE_I64_IMM)) {
				add_code(code,
					"api.mem_st64(cpu, " + from_reg(instr.Stype.rs1) + " + " + from_imm(instr.Stype.signed_imm()) + ", " + from_reg(instr.Stype.rs2) + ");"
				);
			} else {
				throw std::runtime_error("Unhandled store opcode");
			}
			break;
		case RV32I_BRANCH:
			if (IS_HANDLER(ip[i], BRANCH_EQ)) {
				add_branch<W>(code, false, " == ", basepc, i, instr);
			}
			else if (IS_HANDLER(ip[i], BRANCH_NE)) {
				add_branch<W>(code, false, " != ", basepc, i, instr);
			}
			else if (IS_HANDLER(ip[i], BRANCH_LT)) {
				add_branch<W>(code, true, " < ", basepc, i, instr);
			}
			else if (IS_HANDLER(ip[i], BRANCH_GE)) {
				add_branch<W>(code, true, " >= ", basepc, i, instr);
			}
			else if (IS_HANDLER(ip[i], BRANCH_LTU)) {
				add_branch<W>(code, false, " < ", basepc, i, instr);
			}
			else if (IS_HANDLER(ip[i], BRANCH_GEU)) {
				add_branch<W>(code, false, " >= ", basepc, i, instr);
			} else {
				throw std::runtime_error("Unhandled branch opcode");
			}
			break;
		case RV32I_JALR:
			// jump to register + immediate
			if (instr.Itype.rd != 0) {
				add_code(code, from_reg(instr.Itype.rd) + " = " + PCREL(4) + ";\n");
			}
			add_code(code, "api.jump(cpu, " + from_reg(instr.Itype.rs1)
				+ " + " + from_imm(instr.Itype.signed_imm()) + " - 4, " + std::to_string(i) + ");",
				"return;");
			break;
		case RV32I_JAL:
			if (instr.Jtype.rd != 0) {
				add_code(code, from_reg(instr.Jtype.rd) + " = " + PCREL(4) + ";\n");
			}
			add_code(code,
				"api.jump(cpu, " + PCREL(instr.Jtype.jump_offset() - 4) + ", " + std::to_string(i) + ");",
				"}");
			return; // !
		case RV32I_OP_IMM: {
			const auto dst = from_reg(instr.Itype.rd);
			const auto src = from_reg(instr.Itype.rs1);
			switch (instr.Itype.funct3) {
			case 0x0: // ADDI
				emit_op(code, " + ", " += ", instr.Itype.rd, instr.Itype.rs1, from_imm(instr.Itype.signed_imm()));
				break;
			case 0x1: // SLLI
				// SLLI: Logical left-shift 5/6-bit immediate
				if constexpr (W == 8)
					emit_op(code, " << ", " <<= ", instr.Itype.rd, instr.Itype.rs1, std::to_string(instr.Itype.shift64_imm()));
				else
					emit_op(code, " << ", " <<= ", instr.Itype.rd, instr.Itype.rs1, std::to_string(instr.Itype.shift_imm()));
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
				emit_op(code, " ^ ", " ^= ", instr.Itype.rd, instr.Itype.rs1, from_imm(instr.Itype.signed_imm()));
				break;
			case 0x5: // SRLI / SRAI:
				if (LIKELY(!instr.Itype.is_srai())) {
					if constexpr (W == 8)
						emit_op(code, " >> ", " >>= ", instr.Itype.rd, instr.Itype.rs1, std::to_string(instr.Itype.shift64_imm()));
					else
						emit_op(code, " >> ", " >>= ", instr.Itype.rd, instr.Itype.rs1, std::to_string(instr.Itype.shift_imm()));
				} else { // SRAI: preserve the sign bit
					add_code(code,
						"{addr_t bit = 1ul << (sizeof(" + src + ") * 8 - 1);",
						"bool is_signed = (" + src + " & bit) != 0;");
					if constexpr (W == 8) {
						add_code(code,
							"uint32_t shifts = " + std::to_string(instr.Itype.shift64_imm()) + ";",
							dst + " = SRA64(is_signed, shifts, " + src + ");");
					} else {
						add_code(code,
							"uint32_t shifts = " + std::to_string(instr.Itype.shift_imm()) + ";",
							dst + " = SRA32(is_signed, shifts, " + src + ");");
					}
					code += "}";
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
			switch (instr.Rtype.jumptable_friendly_op()) {
			case 0x0: // ADD / SUB
				if (!instr.Rtype.is_f7())
					emit_op(code, " + ", " += ", instr.Rtype.rd, instr.Rtype.rs1, from_reg(instr.Rtype.rs2));
				else
					emit_op(code, " - ", " -= ", instr.Rtype.rd, instr.Rtype.rs1, from_reg(instr.Rtype.rs2));
				break;
			case 0x1: // SLL
				if constexpr (W == 8) {
					add_code(code,
						from_reg(instr.Rtype.rd) + " = " + from_reg(instr.Rtype.rs1) + " << (" + from_reg(instr.Rtype.rs2) + " & 0x3F);");
				} else {
					add_code(code,
						from_reg(instr.Rtype.rd) + " = " + from_reg(instr.Rtype.rs1) + " << (" + from_reg(instr.Rtype.rs2) + " & 0x1F);");
				}
				break;
			case 0x2: // SLT
				add_code(code,
					from_reg(instr.Rtype.rd) + " = ((saddr_t)" + from_reg(instr.Rtype.rs1) + " < (saddr_t)" + from_reg(instr.Rtype.rs2) + ") ? 1 : 0;");
				break;
			case 0x3: // SLTU
				add_code(code,
					from_reg(instr.Rtype.rd) + " = (" + from_reg(instr.Rtype.rs1) + " < " + from_reg(instr.Rtype.rs2) + ") ? 1 : 0;");
				break;
			case 0x4: // XOR
				emit_op(code, " ^ ", " ^= ", instr.Rtype.rd, instr.Rtype.rs1, from_reg(instr.Rtype.rs2));
				break;
			case 0x5: // SRL / SRA
				if (!instr.Rtype.is_f7()) { // SRL
					if constexpr (W == 8) {
						add_code(code,
							from_reg(instr.Rtype.rd) + " = " + from_reg(instr.Rtype.rs1) + " >> (" + from_reg(instr.Rtype.rs2) + " & 0x3F);"); // max 63 shifts!
					} else {
						add_code(code,
							from_reg(instr.Rtype.rd) + " = " + from_reg(instr.Rtype.rs1) + " >> (" + from_reg(instr.Rtype.rs2) + " & 0x1F);"); // max 31 shifts!
					}
				} else { // SRA
					add_code(code,
						"{addr_t bit = 1ul << (sizeof(" + from_reg(instr.Rtype.rs1) + ") * 8 - 1);",
						"bool is_signed = (" + from_reg(instr.Rtype.rs1) + " & bit) != 0;"
					);
					if constexpr (W == 8) {
						add_code(code,
							"uint32_t shifts = " + from_reg(instr.Rtype.rs2) + " & 0x3F;", // max 63 shifts!
							from_reg(instr.Rtype.rd) + " = SRA64(is_signed, shifts, " + from_reg(instr.Rtype.rs1) + ");"
						);
					} else {
						add_code(code,
							"uint32_t shifts = " + from_reg(instr.Rtype.rs2) + " & 0x1F;", // max 31 shifts!
							from_reg(instr.Rtype.rd) + " = SRA32(is_signed, shifts, " + from_reg(instr.Rtype.rs1) + ");"
						);
					}
					add_code(code, "}");
				}
				break;
			case 0x6: // OR
				emit_op(code, " | ", " |= ", instr.Rtype.rd, instr.Rtype.rs1, from_reg(instr.Rtype.rs2));
				break;
			case 0x7: // AND
				emit_op(code, " & ", " &= ", instr.Rtype.rd, instr.Rtype.rs1, from_reg(instr.Rtype.rs2));
				break;
			// extension RV32M / RV64M
			case 0x10: // MUL
				add_code(code,
					from_reg(instr.Rtype.rd) + " = (saddr_t)" + from_reg(instr.Rtype.rs1) + " * (saddr_t)" + from_reg(instr.Rtype.rs2) + ";");
				break;
			case 0x11: // MULH (signed x signed)
				add_code(code,
					(W == 4) ?
					from_reg(instr.Rtype.rd) + " = ((int64_t) " + from_reg(instr.Rtype.rs1) + " * (int64_t) " + from_reg(instr.Rtype.rs2) + ") >> 32u;" :
					"MUL128(&" + from_reg(instr.Rtype.rd) + ", " + from_reg(instr.Rtype.rs1) + ", " + from_reg(instr.Rtype.rs2) + ");"
				);
				break;
			case 0x12: // MULHSU (signed x unsigned)
				add_code(code,
					(W == 4) ?
					from_reg(instr.Rtype.rd) + " = ((int64_t) " + from_reg(instr.Rtype.rs1) + " * (uint64_t)" + from_reg(instr.Rtype.rs2) + ") >> 32u;" :
					"MUL128(&" + from_reg(instr.Rtype.rd) + ", " + from_reg(instr.Rtype.rs1) + ", " + from_reg(instr.Rtype.rs2) + ");"
				);
				break;
			case 0x13: // MULHU (unsigned x unsigned)
				add_code(code,
					(W == 4) ?
					from_reg(instr.Rtype.rd) + " = ((uint64_t) " + from_reg(instr.Rtype.rs1) + " * (uint64_t)" + from_reg(instr.Rtype.rs2) + ") >> 32u;" :
					"MUL128(&" + from_reg(instr.Rtype.rd) + ", " + from_reg(instr.Rtype.rs1) + ", " + from_reg(instr.Rtype.rs2) + ");"
				);
				break;
			case 0x14: // DIV
				// division by zero is not an exception
				if constexpr (W == 8) {
					add_code(code,
						"if (LIKELY(" + from_reg(instr.Rtype.rs2) + " != 0)) {",
						"	if (LIKELY(!(" + from_reg(instr.Rtype.rs1) + " == -9223372036854775808ull && " + from_reg(instr.Rtype.rs2) + " == -1ull)))"
						"		" + from_reg(instr.Rtype.rd) + " = (int64_t)" + from_reg(instr.Rtype.rs1) + " / (int64_t)" + from_reg(instr.Rtype.rs2) + ";",
						"}");
				} else {
					add_code(code,
						"if (LIKELY(" + from_reg(instr.Rtype.rs2) + " != 0)) {",
						"	if (LIKELY(!(" + from_reg(instr.Rtype.rs1) + " == 2147483648 && " + from_reg(instr.Rtype.rs2) + " == 4294967295)))",
						"		" + from_reg(instr.Rtype.rd) + " = (int32_t)" + from_reg(instr.Rtype.rs1) + " / (int32_t)" + from_reg(instr.Rtype.rs2) + ";",
						"}");
				}
				break;
			case 0x15: // DIVU
				add_code(code,
					"if (LIKELY(" + from_reg(instr.Rtype.rs2) + " != 0))",
					from_reg(instr.Rtype.rd) + " = " + from_reg(instr.Rtype.rs1) + " / " + from_reg(instr.Rtype.rs2) + ";"
				);
				break;
			case 0x16: // REM
				if constexpr (W == 8) {
					add_code(code,
					"if (LIKELY(" + from_reg(instr.Rtype.rs2) + " != 0)) {",
					"	if (LIKELY(!(" + from_reg(instr.Rtype.rs1) + " == -9223372036854775808ull && " + from_reg(instr.Rtype.rs2) + " == -1ull)))",
					"		" + from_reg(instr.Rtype.rd) + " = (int64_t)" + from_reg(instr.Rtype.rs1) + " % (int64_t)" + from_reg(instr.Rtype.rs2) + ";",
					"}");
				} else {
					add_code(code,
					"if (LIKELY(" + from_reg(instr.Rtype.rs2) + " != 0)) {",
					"	if (LIKELY(!(" + from_reg(instr.Rtype.rs1) + " == 2147483648 && " + from_reg(instr.Rtype.rs2) + " == 4294967295)))",
					"		" + from_reg(instr.Rtype.rd) + " = (int32_t)" + from_reg(instr.Rtype.rs1) + " % (int32_t)" + from_reg(instr.Rtype.rs2) + ";",
					"}");
				}
				break;
			case 0x17: // REMU
				add_code(code,
				"if (LIKELY(" + from_reg(instr.Rtype.rs2) + " != 0))",
					from_reg(instr.Rtype.rd) + " = " + from_reg(instr.Rtype.rs1) + " % " + from_reg(instr.Rtype.rs2) + ";"
				);
				break;
			}
			break;
		case RV32I_LUI:
			add_code(code,
				from_reg(instr.Utype.rd) + " = (int32_t) " + from_imm(instr.Utype.upper_imm()) + ";"
			);
			break;
		case RV32I_AUIPC:
			add_code(code,
				from_reg(instr.Utype.rd) + " = " + PCREL(instr.Utype.upper_imm()) + ";"
			);
			break;
		case RV32I_FENCE:
			break;
		case RV32I_SYSTEM:
			if (instr.Itype.imm == 1) {
				code += "api.ebreak(cpu, " + std::to_string(i) + ");\n}\n";
			} else {
				code += "api.syscall(cpu, " + std::to_string(i) + ");\n}\n";
			}
			return; // !!
		case RV64I_OP_IMM32: {
			const auto dst = from_reg(instr.Itype.rd);
			const auto src = "(int32_t)" + from_reg(instr.Itype.rs1);
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
					"{const uint32_t shifts = " + from_imm(instr.Itype.shift_imm()) + ";",
					"const bool is_signed = (" + src + " & 0x80000000) != 0;",
					dst + " = " + SIGNEXTW + " SRA32(is_signed, shifts, " + src + ");}");
				}
				break;
			default:
				ILLEGAL_AND_EXIT();
			}
			} break;
		case RV64I_OP32: {
			const auto dst = from_reg(instr.Rtype.rd);
			const auto src1 = "(int32_t)" + from_reg(instr.Rtype.rs1);
			const auto src2 = "(int32_t)" + from_reg(instr.Rtype.rs2);

			switch (instr.Rtype.jumptable_friendly_op()) {
			case 0x0: // ADDW / SUBW
				if (!instr.Rtype.is_f7())
					add_code(code, dst + " = " + SIGNEXTW + " (" + src1 + " + " + src2 + ");");
				else
					add_code(code, dst + " = " + SIGNEXTW + " (" + src1 + " - " + src2 + ");");
				break;
			case 0x1: // SLLW
				add_code(code, dst + " = " + SIGNEXTW + " (" + src1 + " << (" + src2 + " & 0x1F));");
				break;
			case 0x5: // SRLW / SRAW
				if (!instr.Rtype.is_f7()) { // SRL
					add_code(code, dst + " = " + SIGNEXTW + " (" + src1 + " >> (" + src2 + " & 0x1F));");
				} else { // SRAW
					add_code(code,
						"{const bool is_signed = (" + src1 + " & 0x80000000) != 0;",
						"const uint32_t shifts = " + src2 + " & 0x1F;",
						dst + " = " + SIGNEXTW + " (SRA32(is_signed, shifts, " + src1 + ")); }");
				}
				break;
			// M-extension
			case 0x10: // MULW
				add_code(code, dst + " = (int32_t) (" + src1 + " * " + src2 + ");");
				break;
			case 0x14: // DIVW
				// division by zero is not an exception
				add_code(code,
				"if (LIKELY((uint32_t)" + src2 + " != 0))",
				"if (LIKELY(!(" + src1 + " == -2147483648 && " + src2 + " == -1)))",
				dst + " = " + SIGNEXTW + " (" + src1 + " / " + src2 + ");");
				break;
			case 0x15: // DIVUW
				add_code(code,
				"if (LIKELY((uint32_t)" + src2 + " != 0))",
				dst + " = " + SIGNEXTW + " ((uint32_t)" + src1 + " / (uint32_t)" + src2 + ");");
				break;
			case 0x16: // REMW
				add_code(code,
				"if (LIKELY(" + src2 + " != 0))",
				"if (LIKELY(!(" + src1 + " == -2147483648 && " + src2 + " == -1)))",
				dst + " = " + SIGNEXTW + " (" + src1 + " % " + src2 + ");");
				break;
			case 0x17: // REMUW
				add_code(code,
				"if (LIKELY((uint32_t)" + src2 + " != 0))",
				dst + " = " + SIGNEXTW + " ((uint32_t)" + src1 + " % (uint32_t)" + src2 + ");");
				break;
			default:
				ILLEGAL_AND_EXIT();
			}
			} break;
		case RV32F_LOAD: {
			const rv32f_instruction fi { instr };
			const auto addr = from_reg(fi.Itype.rs1) + " + " + from_imm(fi.Itype.signed_imm());
			switch (fi.Itype.funct3) {
			case 0x2: // FLW
				code += "load_fl(&" + from_fpreg(fi.Itype.rd) + ", api.mem_ld32(cpu, " + addr + "));\n";
				break;
			case 0x3: // FLD
				code += "load_dbl(&" + from_fpreg(fi.Itype.rd) + ", api.mem_ld64(cpu, " + addr + "));\n";
				break;
			default:
				ILLEGAL_AND_EXIT();
			}
			} break;
		case RV32F_STORE: {
			const rv32f_instruction fi { instr };
			const auto addr = from_reg(fi.Stype.rs1) + " + " + from_imm(fi.Stype.signed_imm());
			switch (fi.Itype.funct3) {
			case 0x2: // FLW
				code += "api.mem_st32(cpu, " + addr + ", " + from_fpreg(fi.Stype.rs2) + ".f32[0]);\n";
				break;
			case 0x3: // FLD
				code += "api.mem_st64(cpu, " + addr + ", " + from_fpreg(fi.Stype.rs2) + ".i64);\n";
				break;
			default:
				ILLEGAL_AND_EXIT();
			}
			} break;
		case RV32F_FMADD:
		case RV32F_FMSUB:
		case RV32F_FNMADD:
		case RV32F_FNMSUB: {
			const rv32f_instruction fi { instr };
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
			const rv32f_instruction fi { instr };
			const auto dst = from_fpreg(fi.R4type.rd);
			const auto rs1 = from_fpreg(fi.R4type.rs1);
			const auto rs2 = from_fpreg(fi.R4type.rs2);
			if (fi.R4type.funct2 < 0x2) { // fp32 / fp64
			switch (instr.fpfunc()) {
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
			case RV32F__FSGNJ_NX:
				switch (fi.R4type.funct3) {
				case 0x0: // FSGNJ
					if (fi.R4type.rs1 == fi.R4type.rs2) { // FMV rd, rs1
						if (fi.R4type.funct2 == 0x0) // fp32
							code += "set_fl(&" + dst + ", " + rs1 + ".f32[0]);\n";
						else // fp64
							code += "set_dbl(&" + dst + ", " + rs1 + ".f64);\n";
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
			case RV32F__FCVT_SD_DS: {
				if (fi.R4type.funct2 == 0x0) {
					code += "set_fl(&" + dst + ", " + rs1 + ".f64);\n";
				} else if (fi.R4type.funct2 == 0x1) {
					code += "set_dbl(&" + dst + ", " + rs1 + ".f32[0]);\n";
				} else {
					ILLEGAL_AND_EXIT();
				}
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
			} // fpfunc
			} else ILLEGAL_AND_EXIT();
			} break; // RV32F_FPFUNC
		default:
			throw std::runtime_error("Unhandled instruction in code emitter");
		}
	}
	code +=
		"\napi.finish(cpu, " + std::to_string(basepc + 4 * (len-1)) + ", " + std::to_string(len-1) + ");\n"
		"}\n\n";
}
