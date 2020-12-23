template <typename ... Args>
inline void begin_code(std::string& code, rv32i_instruction instr, Args&& ... addendum) {
	code += "{constexpr rv32i_instruction instr {" + std::to_string(instr.whole) + "u};\n";
	([&] {
		code += std::string(addendum) + "\n";
	}(), ...);
}
template <typename ... Args>
inline void begin_code(std::string& code, Args&& ... addendum) {
	code += "{";
	([&] {
		code += std::string(addendum) + "\n";
	}(), ...);
}
template <typename ... Args>
inline void code_block(std::string& code, rv32i_instruction instr, Args&& ... addendum) {
	code += "{constexpr rv32i_instruction instr {" + std::to_string(instr.whole) + "u};\n";
	([&] {
		code += std::string(addendum) + "\n";
	}(), ...);
	code += "}";
}
template <typename ... Args>
inline void code_block(std::string& code, Args&& ... addendum) {
	code += "{";
	([&] {
		code += std::string(addendum) + "\n";
	}(), ...);
	code += "}";
}
template <typename ... Args>
inline void add_code(std::string& code, Args&& ... addendum) {
	([&] {
		code += std::string(addendum) + "\n";
	}(), ...);
}

inline std::string from_reg(int reg) {
	if (reg != 0)
		return "cpu.reg(" + std::to_string(reg) + ")";
	return "0";
}
inline std::string from_imm(int64_t imm) {
	return std::to_string(imm);
}

#define IS_HANDLER(ip, instr) ((ip).first == DECODED_INSTR(instr).handler)
#define IS_FPHANDLER(ip, instr) ((ip).first == DECODED_FLOAT(instr).handler)
#define PCREL(x) std::to_string(basepc + i * 4 + (x))

template <int W>
void CPU<W>::emit(std::string& code, const std::string& func, address_t basepc, instr_pair* ip, size_t len) const
{
	code += "extern \"C\" void " + func + "(CPU<" + std::to_string(W) + ">& cpu, rv32i_instruction) {\n";
	for (size_t i = 0; i < len; i++) {
		const auto& instr = ip[i].second;
		if (IS_HANDLER(ip[i], LOAD_I8_DUMMY)) {
			add_code(code,
				"api.mem_read8(cpu, " + from_reg(instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");"
			);
		}
		else if (IS_HANDLER(ip[i], LOAD_I8)) {
			add_code(code,
				from_reg(instr.Itype.rd) + " = (RVSIGNTYPE(cpu)) (int8_t) api.mem_read8(cpu, " + from_reg(instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");"
			);
		}
		else if (IS_HANDLER(ip[i], LOAD_I16_DUMMY)) {
			add_code(code,
				"api.mem_read16(cpu, " + from_reg(instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");"
			);
		}
		else if (IS_HANDLER(ip[i], LOAD_I16)) {
			add_code(code,
				from_reg(instr.Itype.rd) + " = (RVSIGNTYPE(cpu)) (int16_t) api.mem_read16(cpu, " + from_reg(instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");"
			);
		}
		else if (IS_HANDLER(ip[i], LOAD_I32_DUMMY)) {
			add_code(code,
				"api.mem_read32(cpu, " + from_reg(instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");"
			);
		}
		else if (IS_HANDLER(ip[i], LOAD_I32)) {
			add_code(code,
				from_reg(instr.Itype.rd) + " = (RVSIGNTYPE(cpu)) (int32_t) api.mem_read32(cpu, " + from_reg(instr.Itype.rs1) + " + " + from_imm(instr.Itype.signed_imm()) + ");"
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
		}
		else if (IS_HANDLER(ip[i], STORE_I8) || IS_HANDLER(ip[i], STORE_I8_IMM)) {
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
		}
		else if (IS_HANDLER(ip[i], BRANCH_NE)) {
			add_code(code,
				"if (" + from_reg(instr.Btype.rs1) + " != " + from_reg(instr.Btype.rs2) + ") {",
				"api.jump(cpu, " + PCREL(0 + (int64_t)instr.Btype.signed_imm()) + ");",
				"}api.increment_counter(cpu, " + std::to_string(i) + ");}");
			return; // !
		}
		else if (IS_HANDLER(ip[i], JALR)) {
			// jump to register + immediate
			begin_code(code,
				"const auto address = " + from_reg(instr.Itype.rs1)
					+ " + " + from_imm(instr.Itype.signed_imm()) + ";");
			if (instr.Itype.rd != 0) {
				add_code(code, from_reg(instr.Itype.rd) + " = " + PCREL(4) + ";\n");
			}
			add_code(code, "api.jump(cpu, address - 4);",
				"api.increment_counter(cpu, " + std::to_string(len-1) + ");",
				"}}");
			return; // !
		}
		else if (IS_HANDLER(ip[i], JAL)) {
			code += "{\n";
			if (instr.Jtype.rd != 0) {
				add_code(code, from_reg(instr.Jtype.rd) + " = " + PCREL(4) + ";\n");
			}
			add_code(code,
				"api.jump(cpu, " + PCREL(instr.Jtype.jump_offset() - 4) + ");",
				"api.increment_counter(cpu, " + std::to_string(len-1) + ");",
				"}}");
			return; // !
		}
		else if (IS_HANDLER(ip[i], OP_IMM)
			|| IS_HANDLER(ip[i], OP_IMM_ADDI)
			|| IS_HANDLER(ip[i], OP_IMM_ORI)
			|| IS_HANDLER(ip[i], OP_IMM_ANDI)
			|| IS_HANDLER(ip[i], OP_IMM_LI)
			|| IS_HANDLER(ip[i], OP_IMM_SLLI)) {
			const auto dst = from_reg(instr.Itype.rd);
			const auto src = from_reg(instr.Itype.rs1);
			switch (instr.Itype.funct3) {
			case 0x0: // ADDI
				add_code(code,
					dst + " = " + src + " + " + from_imm(instr.Itype.signed_imm()) + ";");
				break;
			case 0x1: // SLLI
				// SLLI: Logical left-shift 5/6-bit immediate
				if constexpr (W == 8)
					add_code(code,
						dst + " = " + src + " << " + std::to_string(instr.Itype.shift64_imm()) + ";");
				else
					add_code(code,
						dst + " = " + src + " << " + std::to_string(instr.Itype.shift_imm()) + ";");
				break;
			case 0x2: // SLTI:
				// signed less than immediate
				if constexpr (W == 8) {
					add_code(code,
						dst + " = ((int64_t) " + src + " < " + from_imm(instr.Itype.signed_imm()) + ") ? 1 : 0;");
				} else {
					add_code(code,
						dst + " = ((int32_t) " + src + " < " + from_imm(instr.Itype.signed_imm()) + ") ? 1 : 0;");
				}
				break;
			case 0x3: // SLTU:
				add_code(code,
					dst + " = (" + src + " < (unsigned) " + from_imm(instr.Itype.signed_imm()) + ") ? 1 : 0;");
				break;
			case 0x4: // XORI:
				add_code(code,
					dst + " = " + src + " ^ " + from_imm(instr.Itype.signed_imm()) + ";");
				break;
			case 0x5: // SRLI / SRAI:
				if (LIKELY(!instr.Itype.is_srai())) {
					if constexpr (W == 8)
						add_code(code,
							dst + " = " + src + " >> " + std::to_string(instr.Itype.shift64_imm()) + ";");
					else
						add_code(code,
							dst + " = " + src + " >> " + std::to_string(instr.Itype.shift_imm()) + ";");
				} else { // SRAI: preserve the sign bit
					add_code(code,
						"{constexpr auto bit = 1ul << (sizeof(" + src + ") * 8 - 1);",
						"const bool is_signed = (" + src + " & bit) != 0;");
					if constexpr (W == 8) {
						add_code(code,
							"const uint32_t shifts = " + std::to_string(instr.Itype.shift64_imm()) + ";",
							dst + " = RV64I::SRA(is_signed, shifts, " + src + ");");
					} else {
						add_code(code,
							"const uint32_t shifts = " + std::to_string(instr.Itype.shift_imm()) + ";",
							dst + " = RV32I::SRA(is_signed, shifts, " + src + ");");
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
		}
		else if (IS_HANDLER(ip[i], OP)
			|| IS_HANDLER(ip[i], OP_ADD)
			|| IS_HANDLER(ip[i], OP_SUB)) {
			begin_code(code,
				"auto& dst = " + from_reg(instr.Rtype.rd) + ";",
				"const auto src1 = " + from_reg(instr.Rtype.rs1) + ";",
				"const auto src2 = " + from_reg(instr.Rtype.rs2) + ";"
			);
			switch (instr.Rtype.jumptable_friendly_op()) {
			case 0x0: // ADD / SUB
				add_code(code,
					!instr.Rtype.is_f7() ?
					"dst = src1 + src2;" :
					"dst = src1 - src2;"
				);
				break;
			case 0x1: // SLL
				if constexpr (W == 8) {
					add_code(code,
						"dst = src1 << (src2 & 0x3F);");
				} else {
					add_code(code,
						"dst = src1 << (src2 & 0x1F);");
				}
				break;
			case 0x2: // SLT
				add_code(code,
					"dst = (RVTOSIGNED(src1) < RVTOSIGNED(src2)) ? 1 : 0;"
				);
				break;
			case 0x3: // SLTU
				add_code(code,
					"dst = (src1 < src2) ? 1 : 0;");
				break;
			case 0x4: // XOR
				add_code(code,
					"dst = src1 ^ src2;");
				break;
			case 0x5: // SRL / SRA
				if (!instr.Rtype.is_f7()) { // SRL
					if constexpr (W == 8) {
						add_code(code,
							"dst = src1 >> (src2 & 0x3F);"); // max 63 shifts!
					} else {
						add_code(code,
							"dst = src1 >> (src2 & 0x1F);"); // max 31 shifts!
					}
				} else { // SRA
					add_code(code,
						"constexpr auto bit = 1ul << (sizeof(src1) * 8 - 1);",
						"const bool is_signed = (src1 & bit) != 0;"
					);
					if constexpr (W == 8) {
						add_code(code,
							"const uint32_t shifts = src2 & 0x3F;", // max 63 shifts!
							"dst = RV64I::SRA(is_signed, shifts, src1);"
						);
					} else {
						add_code(code,
							"const uint32_t shifts = src2 & 0x1F;", // max 31 shifts!
							"dst = RV32I::SRA(is_signed, shifts, src1);"
						);
					}
				}
				break;
			case 0x6: // OR
				add_code(code,
					"dst = src1 | src2;");
				break;
			case 0x7: // AND
				add_code(code,
					"dst = src1 & src2;");
				break;
			// extension RV32M / RV64M
			case 0x10: // MUL
				add_code(code,
					"dst = RVTOSIGNED(src1) * RVTOSIGNED(src2);"
				);
				break;
			case 0x11: // MULH (signed x signed)
				add_code(code,
					(W == 4) ?
					"dst = ((int64_t) src1 * (int64_t) src2) >> 32u;" :
					"RV64I::MUL128(dst, src1, src2);"
				);
				break;
			case 0x12: // MULHSU (signed x unsigned)
				add_code(code,
					(W == 4) ?
					"dst = ((int64_t) src1 * (uint64_t) src2) >> 32u;" :
					"RV64I::MUL128(dst, src1, src2);"
				);
				break;
			case 0x13: // MULHU (unsigned x unsigned)
				add_code(code,
					(W == 4) ?
					"dst = ((uint64_t) src1 * (uint64_t) src2) >> 32u;" :
					"RV64I::MUL128(dst, src1, src2);"
				);
				break;
			case 0x14: // DIV
				// division by zero is not an exception
				if constexpr (W == 8) {
					add_code(code,
						"if (LIKELY(RVTOSIGNED(src2) != 0)) {",
						"	if (LIKELY(!(src1 == -9223372036854775808ull && src2 == -1ull)))"
						"		dst = RVTOSIGNED(src1) / RVTOSIGNED(src2);",
						"}");
				} else {
					add_code(code,
						"if (LIKELY(RVTOSIGNED(src2) != 0)) {",
						"	if (LIKELY(!(src1 == 2147483648 && src2 == 4294967295)))",
						"		dst = RVTOSIGNED(src1) / RVTOSIGNED(src2);",
						"}");
				}
				break;
			case 0x15: // DIVU
				add_code(code,
					"if (LIKELY(src2 != 0)) dst = src1 / src2;"
				);
				break;
			case 0x16: // REM
				if constexpr (W == 8) {
					add_code(code, R"V0G0N(
					if (LIKELY(src2 != 0)) {
						if (LIKELY(!(src1 == -9223372036854775808ull && src2 == -1ull)))
							dst = RVTOSIGNED(src1) % RVTOSIGNED(src2);
					})V0G0N");
				} else {
					add_code(code, R"V0G0N(
					if (LIKELY(src2 != 0)) {
						if (LIKELY(!(src1 == 2147483648 && src2 == 4294967295)))
							dst = RVTOSIGNED(src1) % RVTOSIGNED(src2);
					})V0G0N");
				}
				break;
			case 0x17: // REMU
				add_code(code,
					"if (LIKELY(src2 != 0))",
						"dst = src1 % src2;"
				);
				break;
			}
			code += "}";
		}
		else if (IS_HANDLER(ip[i], LUI)) {
			add_code(code,
				from_reg(instr.Utype.rd) + " = (int32_t) " + from_imm(instr.Utype.upper_imm()) + ";"
			);
		}
		else if (IS_HANDLER(ip[i], AUIPC)) {
			add_code(code,
				from_reg(instr.Utype.rd) + " = " + PCREL(instr.Utype.upper_imm()) + ";"
			);
		}
		else if (IS_HANDLER(ip[i], NOP)
			|| IS_HANDLER(ip[i], FENCE)) {
			// do nothing
		}
		else if (IS_HANDLER(ip[i], ILLEGAL)
			|| IS_HANDLER(ip[i], UNIMPLEMENTED)) {
			code +=
				"api.trigger_exception(cpu, ILLEGAL_OPCODE);\n";
		}
		else if (IS_FPHANDLER(ip[i], FLW_FLD)) {
			code +=
				"api.trigger_exception(cpu, ILLEGAL_OPCODE);\n";
		}
		else if (IS_FPHANDLER(ip[i], FSW_FSD)) {
			code +=
				"api.trigger_exception(cpu, ILLEGAL_OPCODE);\n";
		}
		else {
			throw std::runtime_error("Unhandled instruction in code emitter");
		}
	}
	code +=
		"\tapi.increment_counter(cpu, " + std::to_string(len-1) + ");\n"
		"\tcpu.increment_pc(" + std::to_string(4 * (len-1)) + ");\n"
		"}\n\n";
}
