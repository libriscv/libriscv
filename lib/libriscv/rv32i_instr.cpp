#include "rv32i.hpp"

#define INSTRUCTION(x, ...) \
		static CPU<4>::instruction_t instr32i_##x { __VA_ARGS__ }
#define DECODED_INSTR(x, instr) { instr32i_##x, instr }

namespace riscv
{
	INSTRUCTION(ILLEGAL,
	[] (auto& cpu, rv32i_instruction instr) {
		// illegal opcode exception
		cpu.trigger_interrupt(ILLEGAL_OPCODE);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		if (instr.opcode() == 0)
			return snprintf(buffer, len, "ILLEGAL OPCODE (Zero, outside executable area?)");
		else
			return snprintf(buffer, len, "ILLEGAL (Unknown)");
	});

	INSTRUCTION(UNIMPLEMENTED,
	[] (auto& cpu, rv32i_instruction instr) {
		// handler
		cpu.trigger_interrupt(UNIMPLEMENTED_INSTRUCTION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		return snprintf(buffer, len, "UNIMPLEMENTED: %#x (%#x)", instr.opcode(), instr.whole);
	});

	INSTRUCTION(LOAD,
	[] (auto& cpu, rv32i_instruction instr)
	{
		const uint32_t reg = instr.Itype.rd;
		if (reg != 0) {
			const auto addr = cpu.reg(instr.Itype.rs1) + instr.Itype.signed_imm();
			const uint32_t type = instr.Itype.funct3;
			if (type == 0) { // LB
				cpu.reg(reg) = cpu.machine().memory.template read<8>(addr);
				// sign-extend 8-bit
				if (cpu.reg(reg) & 0x80) cpu.reg(reg) |= 0xFFFFFF00;
			} else if (type == 1) { // LH
				cpu.reg(reg) = cpu.machine().memory.template read<16>(addr);
				// sign-extend 16-bit
				if (cpu.reg(reg) & 0x8000) cpu.reg(reg) |= 0xFFFF0000;
			} else if (type == 2) { // LW
				cpu.reg(reg) = cpu.machine().memory.template read<32>(addr);
			} else if (type == 4) { // LBU
				// load zero-extended 8-bit value
				cpu.reg(reg) = cpu.machine().memory.template read<8>(addr);
			} else if (type == 5) { // LHU
				// load zero-extended 16-bit value
				cpu.reg(reg) = cpu.machine().memory.template read<16>(addr);
			} else {
				cpu.trigger_interrupt(ILLEGAL_OPERATION);
			}
		}
		else {
			cpu.trigger_interrupt(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		static std::array<const char*, 8> f3 = {"LOADB", "LOADH", "LOADW", "???", "LBU", "LHU", "???", "???"};
		return snprintf(buffer, len, "%s %s, [%s%+d = %#X]",
						f3[instr.Itype.funct3], RISCV::regname(instr.Itype.rd),
						RISCV::regname(instr.Itype.rs1), instr.Itype.signed_imm(),
						cpu.reg(instr.Itype.rs1) + instr.Itype.signed_imm());
	});

	INSTRUCTION(STORE,
	[] (auto& cpu, rv32i_instruction instr)
	{
		const auto value = cpu.reg(instr.Stype.rs2);
		const auto addr  = cpu.reg(instr.Stype.rs1) + instr.Stype.signed_imm();
		const uint32_t type = instr.Stype.funct3;
		if (type == 0) {
			cpu.machine().memory.template write<8, uint8_t>(addr, value);
			//assert(cpu.machine().memory.template read<8> (addr) == (uint8_t) value);
		} else if (type == 1) {
			cpu.machine().memory.template write<16, uint16_t>(addr, value);
			//assert(cpu.machine().memory.template read<16> (addr) == (uint16_t) value);
		} else if (type == 2) {
			cpu.machine().memory.template write<32, uint32_t>(addr, value);
			//assert(cpu.machine().memory.template read<32> (addr) == (uint32_t) value);
		}
		else {
			cpu.trigger_interrupt(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		static std::array<const char*, 4> f3 = {"STOREB", "STOREH", "STOREW", "STORE?"};
		const auto idx = std::min(instr.Stype.funct3, instr.to_word(f3.size()));
		return snprintf(buffer, len, "%s %s, [%s%+d] (%#X)",
						f3[idx], RISCV::regname(instr.Stype.rs2),
						RISCV::regname(instr.Stype.rs1), instr.Stype.signed_imm(),
						cpu.reg(instr.Stype.rs1) + instr.Stype.signed_imm());
	});

	INSTRUCTION(MADD,
	[] (auto& cpu, rv32i_instruction instr) {
		// handler
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		return snprintf(buffer, len, "MADD");
	});

	INSTRUCTION(BRANCH,
	[] (auto& cpu, rv32i_instruction instr) {
		bool comparison;
		const auto& reg1 = cpu.reg(instr.Btype.rs1);
		const auto& reg2 = cpu.reg(instr.Btype.rs2);
		switch (instr.Btype.funct3) {
			case 0x0: // BEQ
				comparison = reg1 == reg2;
				break;
			case 0x1: // BNE
				comparison = reg1 != reg2;
				break;
			case 0x2: // ???
			case 0x3: // ???
				cpu.trigger_interrupt(ILLEGAL_OPERATION);
				break;
			case 0x4: // BLT
				comparison = instr.to_signed(reg1) < instr.to_signed(reg2);
				break;
			case 0x5: // BGE
				comparison = instr.to_signed(reg1) >= instr.to_signed(reg2);
				break;
			case 0x6: // BLTU
				comparison = reg1 < reg2;
				break;
			case 0x7: // BGEU
				comparison = reg1 >= reg2;
				break;
		}
		if (comparison) {
			cpu.jump(cpu.pc() + instr.Btype.signed_imm() - 4);
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// BRANCH compares two registers, BQE = equal taken, BNE = notequal taken
		static std::array<const char*, 8> f3 = {"BEQ", "BNE", "???", "???", "BLT", "BGE", "BLTU", "BGEU"};
		static std::array<const char*, 8> f3z = {"BEQZ", "BNEZ", "???", "???", "BLTZ", "BGEZ", "BLTUZ", "BGEUZ"};
		if (instr.Btype.rs2 != 0) {
			return snprintf(buffer, len, "%s %s, %s => PC%+d (%#X)",
							f3[instr.Btype.funct3],
							RISCV::regname(instr.Btype.rs1),
							RISCV::regname(instr.Btype.rs2),
							instr.Btype.signed_imm(),
							cpu.pc() + instr.Btype.signed_imm());
		} else {
			return snprintf(buffer, len, "%s %s => PC%+d (%#X)",
							f3z[instr.Btype.funct3],
							RISCV::regname(instr.Btype.rs1),
							instr.Btype.signed_imm(),
							cpu.pc() + instr.Btype.signed_imm());
		}
	});

	INSTRUCTION(JALR,
	[] (auto& cpu, rv32i_instruction instr) {
		// return back to where we came from
		// NOTE: returning from _start should exit the machine
		const auto address = cpu.reg(instr.Itype.rs1) + instr.Itype.signed_imm();
		cpu.jump(address - 4);
		if (cpu.machine().verbose_jumps) {
		printf("RET: Returning to %#X <-- %s = %#x%+d\n", address,
				RISCV::regname(instr.Itype.rs1), cpu.reg(instr.Itype.rs1), instr.Itype.signed_imm());
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// RISC-V's RET instruction: return to register + immediate
		const auto address = cpu.reg(instr.Itype.rs1) + instr.Itype.signed_imm();
		return snprintf(buffer, len, "RET %s%+d (%#X)",
						RISCV::regname(instr.Itype.rs1), instr.Itype.signed_imm(), address);
	});

	INSTRUCTION(JAL,
	[] (auto& cpu, rv32i_instruction instr) {
		// Link (rd = PC + 4)
		if (instr.Jtype.rd != 0) {
			cpu.reg(instr.Jtype.rd) = cpu.pc() + 4; // next instruction!
		}
		// And Jump (relative)
		cpu.jump(cpu.pc() + instr.Jtype.jump_offset() - 4);
		if (cpu.machine().verbose_jumps) {
			printf("CALL: %#X <-- %s = %#X\n", cpu.pc(),
					RISCV::regname(instr.Jtype.rd), cpu.reg(instr.Jtype.rd));
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		if (instr.Jtype.rd != 0) {
		return snprintf(buffer, len, "JAL %s, PC%+d (%#X)",
						RISCV::regname(instr.Jtype.rd), instr.Jtype.jump_offset(),
						cpu.pc() + instr.Jtype.jump_offset());
		}
		return snprintf(buffer, len, "JMP PC%+d (%#X)",
						instr.Jtype.jump_offset(), cpu.pc() + instr.Jtype.jump_offset());
	});

	INSTRUCTION(OP_IMM,
	[] (auto& cpu, rv32i_instruction instr)
	{
		if (instr.Itype.rd != 0)
		{
			auto& dst = cpu.reg(instr.Itype.rd);
			const auto& src = cpu.reg(instr.Itype.rs1);
			switch (instr.Itype.funct3) {
			case 0x0:
				// ADDI: Add sign-extended 12-bit immediate
				dst = src + instr.Itype.signed_imm();
				break;
			case 0x1: // SLLI:
				dst = src << instr.Itype.signed_imm();
				break;
			case 0x2: // SLTI:
				dst = instr.to_signed(src) << instr.Itype.signed_imm();
				break;
			case 0x3: // SLTU:
				dst = (src < instr.Itype.signed_imm()) ? 1 : 0;
				break;
			case 0x4: // XORI:
				dst = src ^ instr.Itype.signed_imm();
				break;
			case 0x5: // SRLI: TODO: WRITEME
				dst = src ^ instr.Itype.signed_imm();
				break;
			case 0x6: // ORI:
				dst = src | instr.Itype.signed_imm();
				break;
			case 0x7: // ANDI:
				dst = src & instr.Itype.signed_imm();
				break;
			}

		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		if (instr.Itype.signed_imm() == 0)
		{
			// this is the official NOP instruction (ADDI x0, x0, 0)
			if (instr.Itype.rd == 0 && instr.Itype.rs1 == 0) {
				return snprintf(buffer, len, "NOP");
			}
			static std::array<const char*, 8> func3 = {"MV", "SLL", "SLT", "SLT", "XOR", "SRL", "OR", "AND"};
			return snprintf(buffer, len, "%s %s, %s",
							func3[instr.Itype.funct3],
							RISCV::regname(instr.Itype.rd),
							RISCV::regname(instr.Itype.rs1));

		}
		else if (instr.Itype.rs1 != 0) {
			static std::array<const char*, 8> func3 = {"ADDI", "SLLI", "SLTI", "SLTU", "XORI", "SRLI", "ORI", "ANDI"};
			if (!(instr.Itype.funct3 == 4 && instr.Itype.signed_imm() == -1)) {
				return snprintf(buffer, len, "%s %s, %s%+d",
								func3[instr.Itype.funct3],
								RISCV::regname(instr.Itype.rd),
								RISCV::regname(instr.Itype.rs1),
								instr.Itype.signed_imm());
			} else {
				return snprintf(buffer, len, "NOT %s, %s",
								RISCV::regname(instr.Itype.rd),
								RISCV::regname(instr.Itype.rs1));
			}
		}
		static std::array<const char*, 8> func3 = {"LINT", "SLLI", "SLTI", "SLTU", "XORI", "SRLI", "ORI", "ANDI"};
		return snprintf(buffer, len, "%s %s, %d",
						func3[instr.Itype.funct3],
						RISCV::regname(instr.Itype.rd),
						instr.Itype.signed_imm());
	});

	INSTRUCTION(OP,
	[] (auto& cpu, rv32i_instruction instr)
	{
		if (instr.Rtype.rd != 0)
		{
			auto& dst = cpu.reg(instr.Rtype.rd);
			const auto& src1 = cpu.reg(instr.Rtype.rs1);
			const auto& src2 = cpu.reg(instr.Rtype.rs2);

			switch (instr.Rtype.jumptable_friendly_op()) {
				case 0x0: // ADD / SUB
					dst = src1 + (!instr.Rtype.is_f7() ? src2 : -src2);
					break;
				case 0x1: // SLL
					dst = src1 << src2;
					break;
				case 0x2: // SLT
					dst = (instr.to_signed(src1) < instr.to_signed(src2)) ? 1 : 0;
					break;
				case 0x3: // SLTU
					dst = (src1 < src2) ? 1 : 0;
					break;
				case 0x4: // XOR
					dst = src1 ^ src2;
					break;
				case 0x5: // SRL / SLA TODO: WRITEME
					dst = src1 ^ src2;
					break;
				case 0x6: // OR
					dst = src1 | src2;
					break;
				case 0x7: // AND
					dst = src1 & src2;
					break;
				// extension RV32M
				case 0x10: // MUL
					dst = instr.to_signed(src1) * instr.to_signed(src2);
					break;
				case 0x11: // MULH
					dst = ((int64_t) src1 * (int64_t) src2) >> 32u;
					break;
				case 0x12: // MULHSU
					dst = ((int64_t) src1 * (uint64_t) src2) >> 32u;
					break;
				case 0x13: // MULHU
					dst = ((uint64_t) src1 * (uint64_t) src2) >> 32u;
					break;
				case 0x14: // DIV
					dst = instr.to_signed(src1) / instr.to_signed(src2);
					break;
				case 0x15: // DIVU
					dst = src1 / src2;
					break;
				case 0x16: // REM
					dst = instr.to_signed(src1) % instr.to_signed(src2);
					break;
				case 0x17: // REMU
					dst = src1 % src2;
					break;
			}
		} else {
			cpu.trigger_interrupt(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		if (!instr.Rtype.is_32M())
		{
			static std::array<const char*, 8*2> func3 = {
				"ADD", "SLL", "SLT", "SLTU", "XOR", "SRL", "OR", "AND",
				"SUB", "SLL", "SLT", "SLTU", "XOR", "SRA", "OR", "AND"};
			const int EX = instr.Rtype.is_f7() ? 8 : 0;
			return snprintf(buffer, len, "OP %s <= %s %s %s",
							RISCV::regname(instr.Rtype.rd),
							RISCV::regname(instr.Rtype.rs1),
							func3[instr.Rtype.funct3 + EX],
							RISCV::regname(instr.Rtype.rs2));
		}
		else {
			static std::array<const char*, 8> func3 = {
				"MUL", "MULH", "MULHSU", "MULHU", "DIV", "DIVU", "REM", "REMU"};
			return snprintf(buffer, len, "OP %s <= %s %s %s",
							RISCV::regname(instr.Rtype.rd),
							RISCV::regname(instr.Rtype.rs1),
							func3[instr.Rtype.funct3],
							RISCV::regname(instr.Rtype.rs2));
		}
	});

	INSTRUCTION(SYSTEM,
	[] (auto& cpu, rv32i_instruction instr) {
		// system functions
		if (instr.Itype.funct3 == 0) {
			const int sysn = cpu.reg(RISCV::REG_ECALL);
			switch (instr.Itype.imm)
			{
			case 0: // ECALL
				cpu.machine().system_call(sysn);
				if (cpu.machine().verbose_jumps) {
					printf("SYS ECALL %d returned %d\n", sysn, cpu.reg(RISCV::REG_RETVAL));
				}
				return;
			case 1: // EBREAK
				// do ebreak
				return;
			}
		}
		// if we got here, its an illegal operation!
		cpu.trigger_interrupt(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// system functions
		static std::array<const char*, 2> etype = {"ECALL", "EBREAK"};
		if (instr.Itype.imm < 2 && instr.Itype.funct3 == 0) {
			return snprintf(buffer, len, "SYS %s", etype.at(instr.Itype.imm));
		} else {
			return snprintf(buffer, len, "SYS ???");
		}
	});

	INSTRUCTION(LUI,
	[] (auto& cpu, rv32i_instruction instr) {
		// handler
		cpu.reg(instr.Utype.rd) = instr.Utype.signed_upper();
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		return snprintf(buffer, len, "LUI %s, %#X",
						RISCV::regname(instr.Utype.rd), instr.Utype.signed_upper());
	});

	INSTRUCTION(AUIPC,
	[] (auto& cpu, rv32i_instruction instr) {
		// handler
		cpu.reg(instr.Utype.rd) = cpu.pc() + instr.Utype.signed_upper();
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		return snprintf(buffer, len, "AUIPC %s, PC%+d (%#X)",
						RISCV::regname(instr.Utype.rd), instr.Utype.signed_upper(),
						cpu.pc() + instr.Utype.signed_upper());
	});

	INSTRUCTION(OP_IMM32,
	[] (auto& cpu, rv32i_instruction instr) {
		// handler
		cpu.trigger_interrupt(UNIMPLEMENTED_INSTRUCTION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		return snprintf(buffer, len, "OP_IMM32");
	});

	INSTRUCTION(OP32,
	[] (auto& cpu, rv32i_instruction instr) {
		// handler
		cpu.trigger_interrupt(UNIMPLEMENTED_INSTRUCTION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		return snprintf(buffer, len, "OP_32");
	});
}
