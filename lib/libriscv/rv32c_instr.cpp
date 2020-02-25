#include "rv32c.hpp"
#include "instr_helpers.hpp"

namespace riscv
{
	COMPRESSED_INSTR(C0_ADDI4SPN,
	[] (auto& cpu, rv32i_instruction instr) {
		// if all bits are zero, it's an illegal instruction (by design)
		if (instr.whole != 0x0) {
			auto ci = instr.compressed();
			cpu.cireg(ci.CIW.srd) = cpu.reg(RISCV::REG_SP) + ci.CIW.offset();
			return;
		}
		cpu.trigger_exception(ILLEGAL_OPCODE);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		auto ci = instr.compressed();
		if (UNLIKELY(ci.whole == 0)) {
			return snprintf(buffer, len, "INVALID: All zeroes");
		}
		return snprintf(buffer, len, "C.ADDI4SPN %s, SP+%u (0x%X)",
						RISCV::ciname(ci.CIW.srd), ci.CIW.offset(),
						cpu.reg(RISCV::REG_SP) + ci.CIW.offset());
	});
	// LW, LD, LQ, FLW, FLD, SW, SD, SQ, FSW, FSD
	COMPRESSED_INSTR(C0_REG_LOAD,
	[] (auto& cpu, rv32i_instruction instr) {
		auto ci = instr.compressed();
		auto address = cpu.cireg(ci.CL.srs1) + ci.CL.offset();
		if (ci.CL.funct3 == 0x2) { // LW
			cpu.cireg(ci.CL.srd) = cpu.machine().memory.template read<uint32_t> (address);
			return;
		}
		else if (ci.CL.funct3 == 0x1) { // FLD
			cpu.ciflp(ci.CL.srd).load_u64(
					cpu.machine().memory.template read<uint64_t> (address));
			return;
		}
		else if (ci.CL.funct3 == 0x3) { // FLW
			cpu.ciflp(ci.CL.srd).load_u32(
					cpu.machine().memory.template read<uint32_t> (address));
			return;
		}
		cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		static const std::array<const char*, 4> f3 = {
			"???", "FLD", "LW", "FLW"
		};
		auto ci = instr.compressed();
		return snprintf(buffer, len, "C.%s %s, [%s+%u = 0x%X]",
						f3[ci.CL.funct3], RISCV::ciname(ci.CL.srd),
						RISCV::ciname(ci.CL.srs1), ci.CL.offset(),
						cpu.cireg(ci.CL.srs1) + ci.CL.offset());
	});
	COMPRESSED_INSTR(C0_REG_STORE,
	[] (auto& cpu, rv32i_instruction instr) {
		auto ci = instr.compressed();
		switch (ci.CS.funct3) {
			case 4:
				cpu.trigger_exception(ILLEGAL_OPERATION);
				return;
			case 5: { // C.FSD
				const auto address = cpu.cireg(ci.CSD.srs1) + ci.CSD.offset8();
				const auto value   = cpu.ciflp(ci.CSD.srs2).i64;
				cpu.machine().memory.template write<uint64_t> (address, value);
				} return;
			case 6: { // C.SW
				const auto address = cpu.cireg(ci.CS.srs1) + ci.CS.offset4();
				const auto value   = cpu.cireg(ci.CS.srs2);
				cpu.machine().memory.template write<uint32_t> (address, value);
				} return;
			case 7: { // C.FSW
				const auto address = cpu.cireg(ci.CS.srs1) + ci.CS.offset4();
				const auto value   = cpu.ciflp(ci.CS.srs2).i32[0];
				cpu.machine().memory.template write<uint32_t> (address, value);
				} return;
		}
		cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int
	{
		static const std::array<const char*, 4> f3 = {
			"Reserved instruction", "FSD", "SW", "FSW"
		};
		auto ci = instr.compressed();
		if (ci.CS.funct3 == 0x6) {
		return snprintf(buffer, len, "C.%s %s, [%s%+d]", f3[ci.CS.funct3 - 4],
						RISCV::ciname(ci.CS.srs2),
						RISCV::ciname(ci.CS.srs1), ci.CS.offset4());
		}
		const int offset = (ci.CS.funct3 == 0x7) ? ci.CS.offset4() : ci.CSD.offset8();
		return snprintf(buffer, len, "C.%s %s, [%s%+d]", f3[ci.CS.funct3 - 4],
						RISCV::ciflp(ci.CS.srs2),
						RISCV::ciname(ci.CS.srs1), offset);
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
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int
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
		if (ci.CI.rd != 0) {
			// LI rd, imm[5:0]
			cpu.reg(ci.CI.rd) = ci.CI.signed_imm();
			return;
		}
		// HINTs
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int
	{
		auto ci = instr.compressed();
		return snprintf(buffer, len, "C.LI %s, %+d",
						RISCV::regname(ci.CI.rd), ci.CI.signed_imm());
	});

	COMPRESSED_INSTR(C1_ADDI16SP_LUI,
	[] (auto& cpu, rv32i_instruction instr) {
		auto ci = instr.compressed();
		if (ci.CI.rd == 2) {
			// ADDI16SP rd, imm[17:12]
			cpu.reg(RISCV::REG_SP) += ci.CI16.signed_imm();
			return;
		}
		else if (ci.CI.rd != 0) {
			// LUI rd, imm[17:12]
			cpu.reg(ci.CI.rd) = ci.CI.signed_imm() << 12;
			return;
		}
		// the registers are not allowed
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int
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
	[] (auto& cpu, rv32i_instruction instr)
	{
		auto ci = instr.compressed();
		auto& dst = cpu.cireg(ci.CA.srd);
		switch (ci.CA.funct6 & 0x3)
		{
			case 0: // C.SRLI
				dst = dst >> ci.CAB.shift_imm();
				return;
			case 1: { // C.SRAI (preserve sign)
					const uint32_t shifts = ci.CAB.shift_imm();
					const bool is_signed = (dst & 0x80000000) != 0;
					dst = RV32I::SRA(is_signed, shifts, dst);
					return;
				}
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
		cpu.trigger_exception(ILLEGAL_OPCODE);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int
	{
		auto ci = instr.compressed();
		if ((ci.CA.funct6 & 0x3) < 2) {
			static const std::array<const char*, 2> f3 = {"SRLI", "SRAI"};
			return snprintf(buffer, len, "C.%s %s, %+d",
				f3[ci.CA.funct6 & 0x3], RISCV::ciname(ci.CAB.srd), ci.CAB.shift_imm());
		}
		else if ((ci.CA.funct6 & 0x3) == 2) {
			return snprintf(buffer, len, "C.ANDI %s, %+d",
							RISCV::ciname(ci.CAB.srd), ci.CAB.signed_imm());
		}
		const int op = ci.CA.funct2 | (ci.CA.funct6 & 0x4);
		static const std::array<const char*, 8> f3 = {
			"SUB", "XOR", "OR", "AND", "SUBW", "ADDW", "RESV", "RESV"
		};

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
		else if (ci.CI2.funct3 == 0x1) {
			// FLDSP
			auto address = cpu.reg(RISCV::REG_SP) + ci.CIFLD.offset();
			auto& dst = cpu.registers().getfl(ci.CIFLD.rd);
			dst.load_u64(cpu.machine().memory.template read <uint64_t> (address));
		}
		else if (ci.CI2.funct3 == 0x2 && ci.CI2.rd != 0) {
			// LWSP
			auto address = cpu.reg(RISCV::REG_SP) + ci.CI2.offset();
			cpu.reg(ci.CI2.rd) = cpu.machine().memory.template read <uint32_t> (address);
		}
		else if (ci.CI2.funct3 == 0x3) {
			// FLWSP
			auto address = cpu.reg(RISCV::REG_SP) + ci.CI2.offset();
			auto& dst = cpu.registers().getfl(ci.CI2.rd);
			dst.load_u32(cpu.machine().memory.template read <uint32_t> (address));
		}
		else if (ci.CI.rd == 0) {
			// HINT
		}
		else {
			cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION);
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		auto ci = instr.compressed();
		if (ci.CI2.funct3 == 0x0 && ci.CI2.rd != 0) {
			return snprintf(buffer, len, "C.SLLI %s, %u",
							RISCV::regname(ci.CI.rd), ci.CI.shift_imm());
		}
		else if (ci.CI2.rd != 0) {
			static const std::array<const char*, 4> f3 = {
				"???", "FLDSP", "LWSP", "FLWSP"
			};
			const char* regname = (ci.CI2.funct3 & 1)
			 	? RISCV::flpname(ci.CI2.rd) : RISCV::regname(ci.CI2.rd);
			auto address = (ci.CI2.funct3 != 0x1) ?
						  cpu.reg(RISCV::REG_SP) + ci.CI2.offset()
						: cpu.reg(RISCV::REG_SP) + ci.CIFLD.offset();
			return snprintf(buffer, len, "C.%s %s, [SP+%u] (0x%X)", f3[ci.CI2.funct3],
							regname, ci.CI2.offset(), address);
		}
		return snprintf(buffer, len, "C.HINT %s", RISCV::regname(ci.CI2.rd));
	});
	COMPRESSED_INSTR(C2_SP_STORE,
	[] (auto& cpu, rv32i_instruction instr) {
		auto ci = instr.compressed();
		if (ci.CSS.funct3 == 5) {
			// FSDSP
			auto addr = cpu.reg(RISCV::REG_SP) + ci.CSFSD.offset();
			uint64_t value = cpu.registers().getfl(ci.CSFSD.rs2).i64;
			cpu.machine().memory.template write<uint64_t> (addr, value);
		}
		else if (ci.CSS.funct3 == 6) {
			// SWSP
			auto addr = cpu.reg(RISCV::REG_SP) + ci.CSS.offset(4);
			uint32_t value = cpu.reg(ci.CSS.rs2);
			cpu.machine().memory.template write<uint32_t> (addr, value);
		}
		else if (ci.CSS.funct3 == 7) {
			// FSWSP
			auto addr = cpu.reg(RISCV::REG_SP) + ci.CSS.offset(4);
			uint32_t value = cpu.registers().getfl(ci.CSS.rs2).i32[0];
			cpu.machine().memory.template write<uint32_t> (addr, value);
		}
		else {
			cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION);
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		static const std::array<const char*, 4> f3 = {
			"XXX", "FSDSP", "SWSP", "FSWSP"
		};
		auto ci = instr.compressed();
		auto address = cpu.reg(RISCV::REG_SP) + ci.CSS.offset(4);
		return snprintf(buffer, len, "C.%s [SP%+d], %s (0x%X)",
						f3[ci.CSS.funct3 - 4], ci.CSS.offset(4),
						RISCV::regname(ci.CSS.rs2), address);
	});
	// JR, MV, JALR, ADD
	COMPRESSED_INSTR(C2_VARIOUS,
	[] (auto& cpu, rv32i_instruction instr) {
		auto ci = instr.compressed();
		const bool topbit = ci.whole & (1 << 12);
		if (!topbit && ci.CR.rd != 0 && ci.CR.rs2 == 0)
		{	// JR rd
			cpu.jump(cpu.reg(ci.CR.rd) - 2);
			if (UNLIKELY(cpu.machine().verbose_jumps)) {
				printf(">>> RET 0x%X <-- %s = 0x%X\n", cpu.pc(),
						RISCV::regname(ci.CR.rd), cpu.reg(ci.CR.rd));
			}
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
		else if (!topbit && ci.CR.rd != 0 && ci.CR.rs2 != 0)
		{	// MV rd, rs2
			cpu.reg(ci.CR.rd) = cpu.reg(ci.CR.rs2);
		}
		else if (ci.CR.rd != 0) // ADD rd, rd + rs2
		{
			cpu.reg(ci.CR.rd) += cpu.reg(ci.CR.rs2);
		}
		else if (topbit && ci.CR.rd == 0 && ci.CR.rs2 == 0) // EBREAK
		{
			// its simpler and more flexible to just call a user-provided function
			cpu.machine().system_call(riscv::SYSCALL_EBREAK);
		}
		else {
			cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int
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
		else if (topbit && ci.CR.rd == 0 && ci.CR.rs2 == 0)
			return snprintf(buffer, len, "C.EBREAK");
		return snprintf(buffer, len, "C.HINT");
	});
}
