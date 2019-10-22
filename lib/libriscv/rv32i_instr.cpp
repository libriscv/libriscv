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
			if (type == 0) {
				cpu.reg(reg) = cpu.machine().memory.template read<8>(addr);
			} else if (type == 1) {
				cpu.reg(reg) = cpu.machine().memory.template read<16>(addr);
			} else if (type == 2) {
				cpu.reg(reg) = cpu.machine().memory.template read<32>(addr);
			} else if (type == 4) {
				// TODO: implement LBU
				printf("LBU\n");
			} else if (type == 5) {
				// TODO: implement LHU
				printf("LHU\n");
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
		return snprintf(buffer, len, "%s %s, [%s%+d]",
						f3[instr.Itype.funct3], RISCV::regname(instr.Itype.rd),
						RISCV::regname(instr.Itype.rs1), instr.Itype.signed_imm());
	});

	INSTRUCTION(STORE,
	[] (auto& cpu, rv32i_instruction instr)
	{
		const auto value = cpu.reg(instr.Stype.rs2);
		const auto addr  = cpu.reg(instr.Stype.rs1) + instr.Stype.signed_imm();
		const uint32_t type = instr.Stype.funct3;
		if (type == 0) {
			cpu.machine().memory.template write<8, uint8_t>(addr, value);
		} else if (type == 1) {
			cpu.machine().memory.template write<16, uint16_t>(addr, value);
		} else if (type == 2) {
			cpu.machine().memory.template write<32, uint32_t>(addr, value);
		}
		else {
			cpu.trigger_interrupt(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		static std::array<const char*, 3> f3 = {"STOREB", "STOREH", "STOREW"};
		return snprintf(buffer, len, "%s %s, [%s%+d]",
						f3[instr.Stype.funct3], RISCV::regname(instr.Stype.rs2),
						RISCV::regname(instr.Stype.rs1), instr.Stype.signed_imm());
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
		// handler
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		return snprintf(buffer, len, "BRANCH");
	});

	INSTRUCTION(JALR,
	[] (auto& cpu, rv32i_instruction instr) {
		// return back to where we came from
		// NOTE: returning from _start should exit the machine
		const auto address = cpu.reg(instr.Itype.rs1) + instr.Itype.signed_imm();
		cpu.jump(address);
		if (cpu.machine().verbose_jumps) {
		printf("RET: Returning to %#X <-- %s = %#x%+d\n", cpu.pc(),
				RISCV::regname(instr.Itype.rs1), cpu.reg(instr.Itype.rs1), instr.Itype.signed_imm());
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		const auto address = cpu.reg(instr.Itype.rs1) + instr.Itype.signed_imm();
		return snprintf(buffer, len, "RET %s%+d (%#X)",
						RISCV::regname(instr.Itype.rs1), instr.Itype.signed_imm(), address);
	});

	INSTRUCTION(JAL,
	[] (auto& cpu, rv32i_instruction instr) {
		// Link (rd = PC + 4)
		cpu.reg(instr.Jtype.rd) = cpu.pc() + 4;
		// And Jump (relative)
		cpu.jump(cpu.pc() + instr.Jtype.jump_offset());
		if (cpu.machine().verbose_jumps) {
			printf("CALL: %#X <-- %s = %#X\n", cpu.pc(),
					RISCV::regname(instr.Jtype.rd), cpu.reg(instr.Jtype.rd));
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		return snprintf(buffer, len, "JAL %s, PC%+d (%#X)",
						RISCV::regname(instr.Jtype.rd), instr.Jtype.jump_offset(),
						cpu.pc() + instr.Jtype.jump_offset());
	});

	INSTRUCTION(OP_IMM,
	[] (auto& cpu, rv32i_instruction instr) {
		// handler
		if (instr.Itype.funct3 == 0) {
			// ADDI: Add sign-extended 12-bit immediate
			cpu.reg(instr.Itype.rd) = cpu.reg(instr.Itype.rs1) + instr.Itype.signed_imm();
		}
		else {
			cpu.trigger_interrupt(UNIMPLEMENTED_INSTRUCTION);
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		static std::array<const char*, 3> func3 = {"ADDI", "SLTI", "SLTU"};
		return snprintf(buffer, len, "%s %s, %s, %d",
						func3[instr.Itype.funct3],
						RISCV::regname(instr.Itype.rd),
						RISCV::regname(instr.Itype.rs1),
						instr.Itype.signed_imm());
	});

	INSTRUCTION(OP,
	[] (auto& cpu, rv32i_instruction instr) {
		// handler
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		return snprintf(buffer, len, "OP");
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
					printf("SYSCALL %d returned %d\n", sysn, cpu.reg(RISCV::REG_RETVAL));
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
			return snprintf(buffer, len, "%s", etype.at(instr.Itype.imm));
		} else {
			return snprintf(buffer, len, "SYSTEM ???");
		}
	});

	INSTRUCTION(LUI,
	[] (auto& cpu, rv32i_instruction instr) {
		// handler
		cpu.reg(instr.Utype.rd) = instr.Utype.imm << 12u;
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		return snprintf(buffer, len, "LUI %s, %#x",
						RISCV::regname(instr.Utype.rd), instr.Utype.imm);
	});

	INSTRUCTION(OP_IMM32,
	[] (auto& cpu, rv32i_instruction instr) {
		// handler
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		return snprintf(buffer, len, "OP_IMM32");
	});

	INSTRUCTION(OP32,
	[] (auto& cpu, rv32i_instruction instr) {
		// handler
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		return snprintf(buffer, len, "OP_32");
	});
}
