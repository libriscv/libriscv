#include "rvfd.hpp"

#define FLOAT_INSTR(x, ...) \
		static CPU<4>::instruction_t instr32f_##x { __VA_ARGS__ }
#define DECODED_FLOAT(x) instr32f_##x

namespace riscv
{
	FLOAT_INSTR(FLW_FLD,
	[] (auto& cpu, rv32i_instruction instr)
	{
		rv32f_instruction fi { instr };
		auto addr = cpu.reg(fi.Itype.rs1) + fi.Itype.signed_imm();
		auto& dst = cpu.registers().getfl(fi.Itype.rd);
		switch (fi.Itype.funct3) {
		case 0x2: // FLW
			dst.load_u32(cpu.machine().memory.template read<uint32_t> (addr));
			return;
		case 0x3: // FLD
			dst.load_u64(cpu.machine().memory.template read<uint64_t> (addr));
			return;
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		rv32f_instruction fi { instr };
		static const std::array<const char*, 8> insn {
			"???", "FLH", "FLW", "FLD", "FLQ", "???", "???", "???"
		};
		return snprintf(buffer, len, "%s %s, [%s%+d]",
						insn[fi.Itype.funct3],
                        RISCV::flpname(fi.Itype.rd),
                        RISCV::regname(fi.Stype.rs1),
						fi.Itype.signed_imm());
	});
	FLOAT_INSTR(FSW_FSD,
	[] (auto& cpu, rv32i_instruction instr)
	{
		rv32f_instruction fi { instr };
		auto& src = cpu.registers().getfl(fi.Stype.rs2);
		auto addr = cpu.reg(fi.Stype.rs1) + fi.Stype.signed_imm();
		switch (fi.Itype.funct3) {
		case 0x2: // FSW
			cpu.machine().memory.template write<uint32_t> (addr, src.i32[0]);
			return;
		case 0x3: // FSD
			cpu.machine().memory.template write<uint64_t> (addr, src.i64);
			return;
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		rv32f_instruction fi { instr };
		static const std::array<const char*, 8> insn {
			"???", "FSH", "FSW", "FSD", "FSQ", "???", "???", "???"
		};
		return snprintf(buffer, len, "%s [%s%+d], %s",
						insn[fi.Stype.funct3],
                        RISCV::regname(fi.Stype.rs1),
						fi.Stype.signed_imm(),
						RISCV::flpname(fi.Stype.rs2));
	});

	FLOAT_INSTR(FEQ,
	[] (auto& cpu, rv32i_instruction instr)
	{
		rv32f_instruction fi { instr };
		if (fi.R4type.rd != 0)
		{
			auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
			auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
			auto& dst = cpu.reg(fi.R4type.rd);
			switch (fi.R4type.funct2) {
				case 0x0: // float32
					dst = (rs1.f32[0] == rs2.f32[0]) ? 1 : 0;
					return;
				case 0x1: // float64
					dst = (rs1.f64 == rs2.f64) ? 1 : 0;
					return;
			}
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		rv32f_instruction fi { instr };
		static const std::array<const char*, 4> insn {
			"FEQ.S", "FEQ.D", "???", "FEQ.Q"
		};
		return snprintf(buffer, len, "%s %s %s, %s",
						insn[fi.R4type.funct2],
						RISCV::flpname(fi.R4type.rs1),
						RISCV::flpname(fi.R4type.rs2),
						RISCV::regname(fi.R4type.rd));
	});

	FLOAT_INSTR(FSGNJ_NX,
	[] (auto& cpu, rv32i_instruction instr)
	{
		rv32f_instruction fi { instr };
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		switch (fi.R4type.funct3) {
			case 0x0: // FSGNJ
				switch (fi.R4type.funct2) {
					case 0x0: // float32
						dst.load_u32(rs2.lsign.sign | rs1.lsign.bits);
						return;
					case 0x1: // float64
						dst.i64 = rs2.usign.sign | rs1.usign.bits;
						return;
				}
				break;
			case 0x1: // FSGNJ_N
				switch (fi.R4type.funct2) {
					case 0x0: // float32
						dst.load_u32(~rs2.lsign.sign | rs1.lsign.bits);
						return;
					case 0x1: // float64
						dst.i64 = ~rs2.usign.sign | rs1.usign.bits;
						return;
				}
				break;
			case 0x2: // FSGNJ_X
				switch (fi.R4type.funct2) {
					case 0x0: // float32
						dst.load_u32((rs1.lsign.sign ^ rs2.lsign.sign) | rs1.lsign.bits);
						return;
					case 0x1: // float64
						dst.i64 = (rs1.usign.sign ^ rs2.usign.sign) | rs1.usign.bits;
						return;
				}
				break;
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		rv32f_instruction fi { instr };
		static const std::array<const char*, 4> insn {
			"FSGNJ.S", "FSGNJ.D", "???", "FSGNJ.Q"
		};
		return snprintf(buffer, len, "%s %s %s, %s",
						insn[fi.R4type.funct2],
						RISCV::flpname(fi.R4type.rs1),
						RISCV::flpname(fi.R4type.rs2),
						RISCV::regname(fi.R4type.rd));
	});
}
