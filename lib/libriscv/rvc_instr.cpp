#include "rvc.hpp"
#include "instr_helpers.hpp"
#include <inttypes.h>

namespace riscv
{
	COMPRESSED_INSTR(C0_ADDI4SPN,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		cpu.cireg(ci.CIW.srd) = cpu.reg(REG_SP) + ci.CIW.offset();
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.addi4spn",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	// LW, LD, LQ, FLW, FLD
	COMPRESSED_INSTR(C0_REG_FLD,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		auto address = cpu.cireg(ci.CL.srs1) + ci.CSD.offset8();
		cpu.ciflp(ci.CL.srd).load_u64(
				cpu.machine().memory.template read<uint64_t> (address));
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.fld",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	COMPRESSED_INSTR(C0_REG_LW,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		auto address = cpu.cireg(ci.CL.srs1) + ci.CL.offset();
		cpu.cireg(ci.CL.srd) = (int32_t) cpu.machine().memory.template read<uint32_t> (address);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.lw",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	COMPRESSED_INSTR(C0_REG_LD,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		auto address = cpu.cireg(ci.CSD.srs1) + ci.CSD.offset8();
		cpu.cireg(ci.CSD.srs2) = (int64_t)
				cpu.machine().memory.template read<uint64_t> (address);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.ld",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	COMPRESSED_INSTR(C0_REG_FLW,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		auto address = cpu.cireg(ci.CL.srs1) + ci.CL.offset();
		cpu.ciflp(ci.CL.srd).load_u32(
			cpu.machine().memory.template read<uint32_t> (address));
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.flw",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	// SW, SD, SQ, FSW, FSD
	COMPRESSED_INSTR(C0_REG_FSD,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		const auto address = cpu.cireg(ci.CSD.srs1) + ci.CSD.offset8();
		const auto value   = cpu.ciflp(ci.CSD.srs2).i64;
		cpu.machine().memory.template write<uint64_t> (address, value);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.fsd",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	COMPRESSED_INSTR(C0_REG_SW,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		const auto address = cpu.cireg(ci.CS.srs1) + ci.CS.offset4();
		const auto value   = cpu.cireg(ci.CS.srs2);
		cpu.machine().memory.template write<uint32_t> (address, value);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.sw",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	COMPRESSED_INSTR(C0_REG_SD,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		const auto address = cpu.cireg(ci.CSD.srs1) + ci.CSD.offset8();
		const auto value   = cpu.cireg(ci.CSD.srs2);
		cpu.machine().memory.template write<uint64_t> (address, value);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.sd",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	COMPRESSED_INSTR(C0_REG_FSW,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		const auto address = cpu.cireg(ci.CS.srs1) + ci.CS.offset4();
		const auto value   = cpu.ciflp(ci.CS.srs2).i32[0];
		cpu.machine().memory.template write<uint32_t> (address, value);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.fsw",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	/* Zcb: the byte and halfword loads and stores. These reach the same
	 * memory as their uncompressed counterparts, but only from the eight
	 * compressed registers and with a two-bit offset. */

	COMPRESSED_INSTR(C0_LBU,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		const auto address = cpu.cireg(ci.CZB.srs1) + ci.CZB.byte_offset();
		cpu.cireg(ci.CZB.srd) =
			cpu.machine().memory.template read<uint8_t> (address);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.lbu",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	COMPRESSED_INSTR(C0_LHU,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		const auto address = cpu.cireg(ci.CZB.srs1) + ci.CZB.half_offset();
		cpu.cireg(ci.CZB.srd) =
			cpu.machine().memory.template read<uint16_t> (address);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.lhu",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	COMPRESSED_INSTR(C0_LH,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		const auto address = cpu.cireg(ci.CZB.srs1) + ci.CZB.half_offset();
		cpu.cireg(ci.CZB.srd) = (int16_t)
			cpu.machine().memory.template read<uint16_t> (address);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.lh",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	COMPRESSED_INSTR(C0_SB,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		const auto address = cpu.cireg(ci.CZB.srs1) + ci.CZB.byte_offset();
		const auto value   = cpu.cireg(ci.CZB.srd);
		cpu.machine().memory.template write<uint8_t> (address, value);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.sb",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	COMPRESSED_INSTR(C0_SH,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		const auto address = cpu.cireg(ci.CZB.srs1) + ci.CZB.half_offset();
		const auto value   = cpu.cireg(ci.CZB.srd);
		cpu.machine().memory.template write<uint16_t> (address, value);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.sh",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	COMPRESSED_INSTR(C1_ADDI,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		// C.ADDI (non-hint, not NOP)
		cpu.reg(ci.CI.rd) += ci.CI.signed_imm();
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.addi",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	COMPRESSED_INSTR(C1_JAL,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		cpu.reg(REG_RA) = cpu.pc() + 2; // return instruction
		const auto address = cpu.pc() + ci.CJ.signed_imm();
		cpu.jump(address - 2);
		if constexpr (verbose_branches_enabled) {
			printf(">>> CALL 0x%lX <-- %s = 0x%lX\n", (long) address,
					RISCV::regname(REG_RA), (long) cpu.reg(REG_RA));
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.jal",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	COMPRESSED_INSTR(C1_ADDIW,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		// C.ADDIW rd, imm[5:0]
		const uint32_t src = cpu.reg(ci.CI.rd);
		cpu.reg(ci.CI.rd) = (int32_t) (src + ci.CI.signed_imm());
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.addiw",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	COMPRESSED_INSTR(C1_LI,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		// C.LI rd, imm[5:0]
		cpu.reg(ci.CI.rd) = ci.CI.signed_imm();
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.li",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	COMPRESSED_INSTR(C1_ADDI16SP,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		// C.ADDI16SP rd, imm[17:12]
		cpu.reg(REG_SP) += ci.CI16.signed_imm();
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.addi16sp",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	COMPRESSED_INSTR(C1_LUI,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		// LUI rd, imm[17:12] (sign-extended)
		cpu.reg(ci.CI.rd) = (int32_t) ci.CI.upper_imm();
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.lui",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	COMPRESSED_INSTR(C1_ALU_OPS,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32c_instruction ci { instr };
		auto& dst = cpu.cireg(ci.CA.srd);
		switch (ci.CA.funct6 & 0x3)
		{
			case 0: // C.SRLI
				if constexpr (RVIS64BIT(cpu)) {
					dst = dst >> ci.CAB.shift64_imm();
				} else {
					dst = dst >> ci.CAB.shift_imm();
				}
				return;
			case 1: // C.SRAI (preserve sign)
				dst = (RVSIGNTYPE(cpu))dst >> (ci.CAB.shift64_imm() & (RVXLEN(cpu)-1));
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
					case 4: // C.SUBW
					if constexpr (RVIS64BIT(cpu)) {
						dst = (int32_t) ((uint32_t)dst - (uint32_t)src);
						return;
					}
						break;
					case 5: // C.ADDW
					if constexpr (RVIS64BIT(cpu)) {
						dst = (int32_t) ((uint32_t)dst + (uint32_t)src);
						return;
					}
						break;
					case 6: // Zcb: C.MUL
						dst = RVREGTYPE(cpu)(dst * src);
						return;
					case 7: // Zcb: unary ops, selected by the source field
						switch (ci.CA.srs2)
						{
							case 0: // C.ZEXT.B
								dst = dst & 0xFF;
								return;
							case 1: // C.SEXT.B
								dst = (RVSIGNTYPE(cpu)) (int8_t) dst;
								return;
							case 2: // C.ZEXT.H
								dst = dst & 0xFFFF;
								return;
							case 3: // C.SEXT.H
								dst = (RVSIGNTYPE(cpu)) (int16_t) dst;
								return;
							case 4: // C.ZEXT.W (RV64 and up)
							if constexpr (RVISGE64BIT(cpu)) {
								dst = dst & 0xFFFFFFFF;
								return;
							}
								break;
							case 5: // C.NOT
								dst = ~dst;
								return;
							default: // 6 and 7 are reserved
								break;
						}
						break;
					default:
						break;
				}
		}
		cpu.trigger_exception(ILLEGAL_OPCODE);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc());
	});

	COMPRESSED_INSTR(C1_JUMP,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32c_instruction ci { instr };
		cpu.jump(cpu.pc() + ci.CJ.signed_imm() - 2);
		if constexpr (verbose_branches_enabled) {
			printf(">>> C.JMP 0x%lX\n", (long) cpu.pc() + 2);
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.j",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	COMPRESSED_INSTR(C1_BEQZ,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		// condition: register equals zero
		if (cpu.cireg(ci.CB.srs1) == 0) {
			// branch taken
			cpu.jump(cpu.pc() + ci.CB.signed_imm() - 2);
			if constexpr (verbose_branches_enabled) {
				printf(">>> BRANCH jump to 0x%" PRIX64 "\n", uint64_t(cpu.pc() + 2));
			}
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.beqz",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	COMPRESSED_INSTR(C1_BNEZ,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32c_instruction ci { instr };
		// condition: register not-equal zero
		if (cpu.cireg(ci.CB.srs1) != 0) {
			// branch taken
			cpu.jump(cpu.pc() + ci.CB.signed_imm() - 2);
			if constexpr (verbose_branches_enabled) {
				printf(">>> BRANCH jump to 0x%" PRIX64 "\n", (uint64_t)(cpu.pc() + 2));
			}
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.bnez",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	// C.SLLI, LWSP, LDSP, LQSP, FLWSP, FLDSP
	COMPRESSED_INSTR(C2_SLLI,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32c_instruction ci { instr };
		if constexpr (RVIS64BIT(cpu)) {
			cpu.reg(ci.CI.rd) <<= ci.CI.shift64_imm();
		} else {
			cpu.reg(ci.CI.rd) <<= ci.CI.shift_imm();
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc());
	});

	COMPRESSED_INSTR(C2_FLDSP,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		auto address = cpu.reg(REG_SP) + ci.CIFLD.offset();
		auto& dst = cpu.registers().getfl(ci.CIFLD.rd);
		dst.load_u64(cpu.machine().memory.template read <uint64_t> (address));
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.fldsp",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	COMPRESSED_INSTR(C2_LWSP,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		auto address = cpu.reg(REG_SP) + ci.CI2.offset();
		cpu.reg(ci.CI2.rd) = (int32_t) cpu.machine().memory.template read <uint32_t> (address);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.lwsp",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	COMPRESSED_INSTR(C2_LDSP,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		auto address = cpu.reg(REG_SP) + ci.CIFLD.offset();
		cpu.reg(ci.CIFLD.rd) = (int64_t)
			cpu.machine().memory.template read <uint64_t> (address);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.ldsp",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	COMPRESSED_INSTR(C2_FLWSP,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		auto address = cpu.reg(REG_SP) + ci.CI2.offset();
		auto& dst = cpu.registers().getfl(ci.CI2.rd);
		dst.load_u32(cpu.machine().memory.template read <uint32_t> (address));
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.flwsp",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	// SWSP, SDSP, SQSP, FSWSP, FSDSP
	COMPRESSED_INSTR(C2_FSDSP,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		auto addr = cpu.reg(REG_SP) + ci.CSFSD.offset();
		uint64_t value = cpu.registers().getfl(ci.CSFSD.rs2).i64;
		cpu.machine().memory.template write<uint64_t> (addr, value);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.fsdsp",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	COMPRESSED_INSTR(C2_SWSP,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		auto addr = cpu.reg(REG_SP) + ci.CSS.offset(4);
		uint32_t value = cpu.reg(ci.CSS.rs2);
		cpu.machine().memory.template write<uint32_t> (addr, value);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.swsp",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	COMPRESSED_INSTR(C2_SDSP,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		auto addr = cpu.reg(REG_SP) + ci.CSFSD.offset();
		auto value = cpu.reg(ci.CSFSD.rs2);
		cpu.machine().memory.template write<uint64_t> (addr, value);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.sdsp",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	COMPRESSED_INSTR(C2_FSWSP,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		auto addr = cpu.reg(REG_SP) + ci.CSS.offset(4);
		uint32_t value = cpu.registers().getfl(ci.CSS.rs2).i32[0];
		cpu.machine().memory.template write<uint32_t> (addr, value);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.fswsp",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	// C.JR, C.MV, C.JALR, C.ADD
	COMPRESSED_INSTR(C2_JR,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		cpu.jump(cpu.reg(ci.CR.rd) - 2);
		if constexpr (verbose_branches_enabled) {
			printf(">>> RET 0x%lX <-- %s = 0x%lX\n", (long) cpu.pc(),
				RISCV::regname(ci.CR.rd), (long) cpu.reg(ci.CR.rd));
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.jr",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	COMPRESSED_INSTR(C2_JALR,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		cpu.reg(REG_RA) = cpu.pc() + 0x2;
		cpu.jump(cpu.reg(ci.CR.rd) - 2);
		if constexpr (verbose_branches_enabled) {
			printf(">>> C.JAL RA, 0x%lX <-- %s = 0x%lX\n",
				(long) cpu.reg(REG_RA) - 2,
				RISCV::regname(ci.CR.rd), (long) cpu.reg(ci.CR.rd));
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.jalr",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	COMPRESSED_INSTR(C2_MV,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		cpu.reg(ci.CR.rd) = cpu.reg(ci.CR.rs2);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.mv",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	COMPRESSED_INSTR(C2_ADD,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const rv32c_instruction ci { instr };
		cpu.reg(ci.CR.rd) += cpu.reg(ci.CR.rs2);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.add",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});

	COMPRESSED_INSTR(C2_EBREAK,
	[] (auto& cpu, rv32i_instruction) RVINSTR_COLDATTR {
		cpu.machine().ebreak();
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVCDISASM::as(buffer, len, "c.ebreak",
			RVCDISASM::any(buffer, len, rv32c_instruction { instr },
			RVISGE64BIT(cpu), cpu.pc()));
	});
}
