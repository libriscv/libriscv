#define IS_OPCODE(instr, opcode) (instr.opcode() == opcode)
#define IS_HANDLER(ip, instr) ((ip).first == DECODED_INSTR(instr).handler)
#define IS_FPHANDLER(ip, instr) ((ip).first == DECODED_FLOAT(instr).handler)
#define PCREL(x) std::to_string((address_t) (basepc + i * 4 + (x)))

template <typename ... Args>
inline void add_code(std::string& code, Args&& ... addendum) {
	([&] {
		code += std::string(addendum) + "\n";
	}(), ...);
}
inline std::string from_reg(int reg) {
	if (reg != 0)
		return "cpu->regs[" + std::to_string(reg) + "]";
	return "0";
}
inline std::string from_fpreg(int reg) {
	return "cpu->fpreg[" + std::to_string(reg) + "]";
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
		"if ((saddress_t)" + from_reg(instr.Btype.rs1) + op + " (saddress_t)" + from_reg(instr.Btype.rs2) + ") {"),
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
	code += "extern void " + func + "(ThinCPU* cpu) {\n";
	for (size_t i = 0; i < len; i++) {
		const auto& instr = ip[i].second;
		switch (instr.opcode()) {
		case RV32I_LOAD:
			if (IS_HANDLER(ip[i], LOAD_I8_DUMMY)) {
				add_code(code,
					"api.mem_read8(cpu, " + from_reg(instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");"
				);
			}
			else if (IS_HANDLER(ip[i], LOAD_I8)) {
				add_code(code,
					from_reg(instr.Itype.rd) + " = (saddress_t) (int8_t) api.mem_read8(cpu, " + from_reg(instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");"
				);
			}
			else if (IS_HANDLER(ip[i], LOAD_I16_DUMMY)) {
				add_code(code,
					"api.mem_read16(cpu, " + from_reg(instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");"
				);
			}
			else if (IS_HANDLER(ip[i], LOAD_I16)) {
				add_code(code,
					from_reg(instr.Itype.rd) + " = (saddress_t) (int16_t) api.mem_read16(cpu, " + from_reg(instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");"
				);
			}
			else if (IS_HANDLER(ip[i], LOAD_I32_DUMMY)) {
				add_code(code,
					"api.mem_read32(cpu, " + from_reg(instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");"
				);
			}
			else if (IS_HANDLER(ip[i], LOAD_I32)) {
				add_code(code,
					from_reg(instr.Itype.rd) + " = (saddress_t) (int32_t) api.mem_read32(cpu, " + from_reg(instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");"
				);
			}
			else if (IS_HANDLER(ip[i], LOAD_U8)) {
				add_code(code,
					from_reg(instr.Itype.rd) + " = api.mem_read8(cpu, " + from_reg(instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");"
				);
			}
			else if (IS_HANDLER(ip[i], LOAD_U16)) {
				add_code(code,
					from_reg(instr.Itype.rd) + " = api.mem_read16(cpu, " + from_reg(instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");"
				);
			}
			else if (IS_HANDLER(ip[i], LOAD_U32)) {
				add_code(code,
					from_reg(instr.Itype.rd) + " = api.mem_read32(cpu, " + from_reg(instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");"
				);
			}
			else if (IS_HANDLER(ip[i], LOAD_U64)) {
				add_code(code,
					from_reg(instr.Itype.rd) + " = api.mem_read64(cpu, " + from_reg(instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");"
				);
			}
			else if (IS_HANDLER(ip[i], LOAD_U64_DUMMY)) {
				add_code(code,
					"api.mem_read64(cpu, " + from_reg(instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");"
				);
			} else {
				throw std::runtime_error("Unhandled load opcode");
			}
			break;
		case RV32I_STORE:
			if (IS_HANDLER(ip[i], STORE_I8) || IS_HANDLER(ip[i], STORE_I8_IMM)) {
				add_code(code,
					"api.mem_write8(cpu, " + from_reg(instr.Stype.rs1) + " + " + from_imm(instr.Stype.signed_imm()) + ", " + from_reg(instr.Stype.rs2) + ");"
				);
			}
			else if (IS_HANDLER(ip[i], STORE_I16_IMM)) {
				add_code(code,
					"api.mem_write16(cpu, " + from_reg(instr.Stype.rs1) + " + " + from_imm(instr.Stype.signed_imm()) + ", " + from_reg(instr.Stype.rs2) + ");"
				);
			}
			else if (IS_HANDLER(ip[i], STORE_I32_IMM)) {
				add_code(code,
					"api.mem_write32(cpu, " + from_reg(instr.Stype.rs1) + " + " + from_imm(instr.Stype.signed_imm()) + ", " + from_reg(instr.Stype.rs2) + ");"
				);
			}
			else if (IS_HANDLER(ip[i], STORE_I64_IMM)) {
				add_code(code,
					"api.mem_write64(cpu, " + from_reg(instr.Stype.rs1) + " + " + from_imm(instr.Stype.signed_imm()) + ", " + from_reg(instr.Stype.rs2) + ");"
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
					dst + " = ((saddress_t)" + src + " < " + from_imm(instr.Itype.signed_imm()) + ") ? 1 : 0;");
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
						"{address_t bit = 1ul << (sizeof(" + src + ") * 8 - 1);",
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
					from_reg(instr.Rtype.rd) + " = ((saddress_t)" + from_reg(instr.Rtype.rs1) + " < (saddress_t)" + from_reg(instr.Rtype.rs2) + ") ? 1 : 0;");
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
						"{address_t bit = 1ul << (sizeof(" + from_reg(instr.Rtype.rs1) + ") * 8 - 1);",
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
					from_reg(instr.Rtype.rd) + " = (saddress_t)" + from_reg(instr.Rtype.rs1) + " * (saddress_t)" + from_reg(instr.Rtype.rs2) + ";");
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
		case RV32F_LOAD: {
			const rv32f_instruction fi { instr };
			const auto addr = from_reg(fi.Itype.rs1) + " + " + from_imm(fi.Itype.signed_imm());
			switch (fi.Itype.funct3) {
			case 0x2: // FLW
				code += "load_float(&" + from_fpreg(fi.Itype.rd) + ", api.mem_read32(cpu, " + addr + "));";
				break;
			case 0x3: // FLD
				code += "load_double(&" + from_fpreg(fi.Itype.rd) + ", api.mem_read64(cpu, " + addr + "));";
				break;
			default:
				code += "api.trigger_exception(cpu, ILLEGAL_OPCODE);\n";
			}
			} break;
		case RV32F_STORE: {
			const rv32f_instruction fi { instr };
			const auto addr = from_reg(fi.Itype.rs1) + " + " + from_imm(fi.Itype.signed_imm());
			switch (fi.Itype.funct3) {
			case 0x2: // FLW
				code += "api.mem_write32(cpu, " + addr + ", " + from_fpreg(fi.Itype.rd) + ".f32[0]);";
				break;
			case 0x3: // FLD
				code += "api.mem_write64(cpu, " + addr + ", " + from_fpreg(fi.Itype.rd) + ".i64);";
				break;
			default:
				code += "api.trigger_exception(cpu, ILLEGAL_OPCODE);\n";
			}
			} break;
		default:
			throw std::runtime_error("Unhandled instruction in code emitter");
		}
	}
	code +=
		"\napi.finish_block(cpu, " + std::to_string(basepc + 4 * (len-1)) + ", " + std::to_string(len-1) + ");\n"
		"}\n\n";
}
