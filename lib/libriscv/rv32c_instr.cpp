#include "rv32c.hpp"

#define COMPRESSED_INSTR(x, ...) INSTRUCTION(x, __VA_ARGS__)
#define DECODED_COMPR(x) DECODED_INSTR(x)
#define CI_CODE(x, y) ((x << 13) | (y))
#define CIC2_CODE(x, y) ((x << 12) | (y))

namespace riscv
{
	// LW, LD, LQ, FLW, FLD, SW, SD, SQ, FSW, FSD
	COMPRESSED_INSTR(C0_ADDI4SPN,
	[] (auto& cpu, rv32i_instruction instr) {
		auto ci = instr.compressed();
		cpu.cireg(ci.CIW.srd) = cpu.reg(RISCV::REG_SP) + ci.CIW.offset();
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		auto ci = instr.compressed();
		return snprintf(buffer, len, "C.ADDI4SPN %s, SP+%u (0x%X)",
						RISCV::ciname(ci.CIW.srd), ci.CIW.offset(),
						cpu.reg(RISCV::REG_SP) + ci.CIW.offset());
	});
	COMPRESSED_INSTR(C0_REG_LOAD,
	[] (auto& cpu, rv32i_instruction instr) {
		auto ci = instr.compressed();
		auto address = cpu.cireg(ci.CL.srs1) + ci.CL.signed_imm();
		if (ci.CL.funct3 == 0x2) {
			cpu.cireg(ci.CL.srd) = cpu.machine().memory.template read<uint32_t> (address);
			return;
		}
		cpu.trigger_interrupt(UNIMPLEMENTED_INSTRUCTION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		static std::array<const char*, 4> f3 = {
			"???", "FLD", "LW", "FLW"
		};
		auto ci = instr.compressed();
		return snprintf(buffer, len, "C.%s %s, [%s%+d = 0x%X]",
						f3[ci.CL.funct3], RISCV::ciname(ci.CL.srd),
						RISCV::ciname(ci.CL.srs1), ci.CL.signed_imm(),
						cpu.cireg(ci.CL.srs1) + ci.CL.signed_imm());
	});
	COMPRESSED_INSTR(C0_REG_STORE,
	[] (auto& cpu, rv32i_instruction instr) {
		auto ci = instr.compressed();
		switch (ci.CS.funct3) {
			case 4:
				cpu.trigger_interrupt(ILLEGAL_OPERATION);
				return;
			case 5:
				// TODO: implement me
				printf("FSD instruction ignored (FIXME)\n");
				return;
			case 6: {
				const auto address = cpu.cireg(ci.CS.srs1) + ci.CS.offset4();
				const auto value   = cpu.cireg(ci.CS.srs2);
				cpu.machine().memory.template write<uint32_t> (address, value);
				} return;
			case 7:
				// TODO: implement me
				printf("FSW instruction ignored (FIXME)\n");
				return;
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		static std::array<const char*, 4> f3 = {
			"Reserved instruction", "FSD", "SW", "FSW"
		};
		auto ci = instr.compressed();
		return snprintf(buffer, len, "C.%s [%s%+d], %s", f3[ci.CS.funct3 - 4],
						RISCV::ciname(ci.CS.srs1), ci.CS.offset4(),
						RISCV::ciname(ci.CS.srs2));
	});

	COMPRESSED_INSTR(C1_NOP_ADDI,
	[] (auto& cpu, rv32i_instruction instr) {
		auto ci = instr.compressed();
		// ADDI (non-hint, not NOP)
		if (ci.CI.rd != 0)
		{
			cpu.reg(ci.CI.rd) += ci.CI.signed_imm();
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		auto ci = instr.compressed();
		if (ci.CI.rd != 0) {
			return snprintf(buffer, len, "C.ADDI %s, %d",
							RISCV::regname(ci.CI.rd), ci.CI.signed_imm());
		}
		if (ci.CI.imm1 != 0 || ci.CI.imm2 != 0)
			return snprintf(buffer, len, "C.HINT");
		return snprintf(buffer, len, "C.NOP");
	});

	COMPRESSED_INSTR(C1_JAL,
	[] (auto& cpu, rv32i_instruction instr) {
		auto ci = instr.compressed();
		cpu.reg(RISCV::REG_RA) = cpu.pc() + 2; // return instruction
		const auto address = cpu.pc() + ci.CJ.signed_imm();
		cpu.jump(address - 2);
		if (UNLIKELY(cpu.machine().verbose_jumps)) {
			printf(">>> CALL 0x%X <-- %s = 0x%X\n", address,
					RISCV::regname(RISCV::REG_RA), cpu.reg(RISCV::REG_RA));
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		auto ci = instr.compressed();
		return snprintf(buffer, len, "C.JAL %s, PC%+d (0x%X)",
						RISCV::regname(RISCV::REG_RA),
						ci.CJ.signed_imm(), cpu.pc() + ci.CJ.signed_imm());
	});

	COMPRESSED_INSTR(C1_LI,
	[] (auto& cpu, rv32i_instruction instr) {
		auto ci = instr.compressed();
		cpu.reg(ci.CI.rd) = ci.CI.signed_imm();
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		auto ci = instr.compressed();
		return snprintf(buffer, len, "C.LI %s, %+d",
						RISCV::regname(ci.CI.rd), ci.CI.signed_imm());
	});

	COMPRESSED_INSTR(C1_ADDI16SP_LUI,
	[] (auto& cpu, rv32i_instruction instr) {
		auto ci = instr.compressed();
		if (ci.CI.rd != 0 && ci.CI.rd != 2) {
			// LUI rd, imm[17:12]
			cpu.reg(ci.CI.rd) = ci.CI.signed_imm() << 12;
			return;
		}
		else if (ci.CI.rd == 2) {
			// ADDI16SP rd, imm[17:12]
			cpu.reg(RISCV::REG_SP) += ci.CI16.signed_imm();
			return;
		}
		// the registers are not allowed
		cpu.trigger_interrupt(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		auto ci = instr.compressed();
		if (ci.CI.rd != 0 && ci.CI.rd != 2) {
			return snprintf(buffer, len, "C.LUI %s, 0x%X",
							RISCV::regname(ci.CI.rd), ci.CI.signed_imm() << 12);
		} else if (ci.CI.rd == 2) {
			return snprintf(buffer, len, "C.ADDI16SP %s, %+d",
							RISCV::regname(ci.CI.rd), ci.CI16.signed_imm());
		}
		return snprintf(buffer, len, "C.LUI (Invalid values)");
	});

	COMPRESSED_INSTR(C1_ALU_OPS,
	[] (auto& cpu, rv32i_instruction instr) {
		// TODO: implement me
		auto ci = instr.compressed();
		auto& dst = cpu.cireg(ci.CA.srd);
		switch (ci.CA.funct6 & 0x3)
		{
			case 0: // C.SRLI
				dst = dst >> ci.CAB.shift_imm();
				return;
			case 1: // C.SRAI (preserve sign)
				dst = ((dst & 0x7FFFFFFF) >> ci.CAB.shift_imm()) | (dst & 0x80000000);
				return;
			case 2: // C.ANDI
				dst = dst & ci.CAB.signed_imm();
				return;
			case 3: // more ops
				const auto& src = cpu.cireg(ci.CA.srs2);
				switch (ci.CA.funct2 | (ci.CA.funct6 & 0x4))
				{
					case 0: // C.SUB
						dst = dst - src;
						return;
					case 1: // C.XOR
						dst = dst ^ src;
						return;
					case 2: // C.OR
						dst = dst | src;
						return;
					case 3: // C.AND
						dst = dst & src;
						return;
					case 0x4: // C.SUBW
					case 0x5: // C.ADDW
					case 0x6: // RESERVED
					case 0x7: // RESERVED
						break;
				}
		}
		cpu.trigger_interrupt(UNIMPLEMENTED_INSTRUCTION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		auto ci = instr.compressed();
		if ((ci.CA.funct6 & 0x3) < 2) {
			static std::array<const char*, 2> f3 = {"SRLI", "SRAI"};
			return snprintf(buffer, len, "C.%s %s, %+d",
				f3[ci.CA.funct6 & 0x3], RISCV::ciname(ci.CAB.srd), ci.CAB.shift_imm());
		}
		else if ((ci.CA.funct6 & 0x3) == 2) {
			return snprintf(buffer, len, "C.ANDI %s, %+d",
							RISCV::ciname(ci.CAB.srd), ci.CAB.signed_imm());
		}
		const int op = ci.CA.funct2 | (ci.CA.funct6 & 0x4);
		static std::array<const char*, 8> f3 = {"SUB", "XOR", "OR", "AND", "SUBW", "ADDW", "RESV", "RESV"};

		return snprintf(buffer, len, "C.%s %s, %s", f3[op],
						RISCV::ciname(ci.CA.srd), RISCV::ciname(ci.CA.srs2));
	});

	COMPRESSED_INSTR(C1_JUMP,
	[] (auto& cpu, rv32i_instruction instr) {
		auto ci = instr.compressed();
		cpu.jump(cpu.pc() + ci.CJ.signed_imm() - 2);
		if (UNLIKELY(cpu.machine().verbose_jumps)) {
			printf(">>> C.JMP 0x%X\n", cpu.pc() + 2);
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		auto ci = instr.compressed();
		return snprintf(buffer, len, "C.JMP 0x%X", cpu.pc() + ci.CJ.signed_imm());
	});

	COMPRESSED_INSTR(C1_BEQZ,
	[] (auto& cpu, rv32i_instruction instr) {
		auto ci = instr.compressed();
		// condition: register equals zero
		if (cpu.cireg(ci.CB.srs1) == 0) {
			// branch taken
			cpu.jump(cpu.pc() + ci.CB.signed_imm() - 2);
			if (UNLIKELY(cpu.machine().verbose_jumps)) {
				printf(">>> BRANCH jump to 0x%X\n", cpu.pc() + 2);
			}
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		auto ci = instr.compressed();
		return snprintf(buffer, len, "C.BEQZ %s, PC%+d (0x%X)",
						RISCV::ciname(ci.CB.srs1), ci.CB.signed_imm(),
						cpu.pc() + ci.CB.signed_imm());
	});

	COMPRESSED_INSTR(C1_BNEZ,
	[] (auto& cpu, rv32i_instruction instr) {
		auto ci = instr.compressed();
		// condition: register not-equal zero
		if (cpu.cireg(ci.CB.srs1) != 0) {
			// branch taken
			cpu.jump(cpu.pc() + ci.CB.signed_imm() - 2);
			if (UNLIKELY(cpu.machine().verbose_jumps)) {
				printf(">>> BRANCH jump to 0x%X\n", cpu.pc() + 2);
			}
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		auto ci = instr.compressed();
		return snprintf(buffer, len, "C.BNEZ %s, PC%+d (0x%X)",
						RISCV::ciname(ci.CB.srs1), ci.CB.signed_imm(),
						cpu.pc() + ci.CB.signed_imm());
	});

	// LWSP, LDSP, LQSP, FLWSP, FLDSP, SWSP, SDSP, SQSP, FSWSP, FSDSP
	COMPRESSED_INSTR(C2_SP_LOAD,
	[] (auto& cpu, rv32i_instruction instr) {
		auto ci = instr.compressed();
		if (ci.CI.funct3 == 0x0 && ci.CI.rd != 0) {
			// SLLI
			cpu.reg(ci.CI.rd) <<= ci.CI.shift_imm();
		}
		else if (ci.CI2.funct3 == 0x2 && ci.CI2.rd != 0) {
			// LWSP
			auto address = cpu.reg(RISCV::REG_SP) + ci.CI2.offset();
			cpu.reg(ci.CI2.rd) = cpu.machine().memory.template read <uint32_t> (address);
		}
		else if (ci.CI.rd == 0) {
			// HINT
		}
		else {
			cpu.trigger_interrupt(UNIMPLEMENTED_INSTRUCTION);
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		static std::array<const char*, 4> f3 = {
			"SLLI", "FLDSP", "LWSP", "FLWSP"
		};
		auto ci = instr.compressed();
		if (ci.CI2.funct3 == 0x0 && ci.CI2.rd != 0) {
			return snprintf(buffer, len, "C.SLLI %s, %u",
							RISCV::regname(ci.CI.rd), ci.CI.shift_imm());
		}
		else if (ci.CI2.rd != 0) {
			auto address = cpu.reg(RISCV::REG_SP) + ci.CI2.offset();
			return snprintf(buffer, len, "C.%s %s, [SP+%u] (0x%X)", f3[ci.CI2.funct3],
							RISCV::regname(ci.CI2.rd), ci.CI2.offset(), address);
		}
		return snprintf(buffer, len, "C.HINT %s?, %s", f3[ci.CI2.funct3],
						RISCV::regname(ci.CI2.rd));
	});
	COMPRESSED_INSTR(C2_SP_STORE,
	[] (auto& cpu, rv32i_instruction instr) {
		auto ci = instr.compressed();
		if (ci.CSS.funct3 == 5) {
			// FSDSP
			printf("TODO: implement FSDSP (FIXME)\n");
		}
		else if (ci.CSS.funct3 == 6) {
			// SWSP
			uint32_t addr = cpu.reg(RISCV::REG_SP) + ci.CSS.offset4();
			uint32_t value = cpu.reg(ci.CSS.rs2);
			cpu.machine().memory.template write<uint32_t> (addr, value);
		}
		else if (ci.CSS.funct3 == 7) {
			// FSWSP
			auto val = cpu.reg(RISCV::REG_SP) + ci.CSS.offset4();
			//cpu.machine().memory.write<uint64_t>
			printf("TODO: implement FSWSP (FIXME)\n");
		}
		else {
			cpu.trigger_interrupt(UNIMPLEMENTED_INSTRUCTION);
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		static std::array<const char*, 4> f3 = {
			"XXX", "FSDSP", "SWSP", "FSWSP"
		};
		auto ci = instr.compressed();
		auto address = cpu.reg(RISCV::REG_SP) + ci.CSS.offset4();
		return snprintf(buffer, len, "C.%s [SP%+d], %s (0x%X)",
						f3[ci.CSS.funct3 - 4], ci.CSS.offset4(),
						RISCV::regname(ci.CSS.rs2), address);
	});
	// JR, MV, JALR, ADD
	COMPRESSED_INSTR(C2_VARIOUS,
	[] (auto& cpu, rv32i_instruction instr) {
		auto ci = instr.compressed();
		const bool topbit = ci.whole & (1 << 12);
		if (!topbit && ci.CR.rs2 == 0 && ci.CR.rd != 0)
		{	// JR rd
			cpu.jump(cpu.reg(ci.CR.rd) - 2);
			if (UNLIKELY(cpu.machine().verbose_jumps)) {
				printf(">>> RET 0x%X <-- %s = 0x%X\n", cpu.pc(),
						RISCV::regname(ci.CR.rd), cpu.reg(ci.CR.rd));
			}
		}
		else if (!topbit && ci.CR.rs2 != 0 && ci.CR.rd != 0)
		{	// MV rd, rs2
			cpu.reg(ci.CR.rd) = cpu.reg(ci.CR.rs2);
		}
		else if (topbit && ci.CR.rd != 0 && ci.CR.rs2 == 0)
		{	// JALR ra, rd+0
			cpu.reg(RISCV::REG_RA) = cpu.pc() + 0x2;
			cpu.jump(cpu.reg(ci.CR.rd) - 2);
			if (UNLIKELY(cpu.machine().verbose_jumps)) {
				printf(">>> C.JAL RA, 0x%X <-- %s = 0x%X\n", cpu.reg(RISCV::REG_RA) - 2,
						RISCV::regname(ci.CR.rd), cpu.reg(ci.CR.rd));
			}
		}
		else if (ci.CR.rd != 0) // ADD rd, rd + rs2
		{
			cpu.reg(ci.CR.rd) += cpu.reg(ci.CR.rs2);
		}
		else {
			cpu.trigger_interrupt(UNIMPLEMENTED_INSTRUCTION);
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		auto ci = instr.compressed();
		const bool topbit = ci.whole & (1 << 12);
		if (!topbit && ci.CR.rs2 == 0 && ci.CR.rd != 0) {
			if (ci.CR.rd == RISCV::REG_RA)
				return snprintf(buffer, len, "C.RET");
			return snprintf(buffer, len, "C.JR %s", RISCV::regname(ci.CR.rd));
		} else if (!topbit && ci.CR.rs2 != 0 && ci.CR.rd != 0)
			return snprintf(buffer, len, "C.MV %s, %s",
							RISCV::regname(ci.CR.rd), RISCV::regname(ci.CR.rs2));
		else if (topbit && ci.CR.rd != 0 && ci.CR.rs2 == 0)
			return snprintf(buffer, len, "C.JALR RA, %s", RISCV::regname(ci.CR.rd));
		else if (ci.CR.rd != 0)
			return snprintf(buffer, len, "C.ADD %s, %s + %s", RISCV::regname(ci.CR.rd),
							RISCV::regname(ci.CR.rd), RISCV::regname(ci.CR.rs2));
		return snprintf(buffer, len, "C.HINT");
	});

	COMPRESSED_INSTR(C2_EBREAK,
	[] (auto& cpu, rv32i_instruction) {
		// its simpler and more flexible to just call a user-provided function
		cpu.machine().system_call(0);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		auto ci = instr.compressed();
		return snprintf(buffer, len, "C.EBREAK");
	});
}
