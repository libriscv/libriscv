#include "rvfd.hpp"
#include "instr_helpers.hpp"

namespace riscv
{
	FLOAT_INSTR(FLW,
	[] (auto& cpu, rv32i_instruction instr)
	{
		const rv32f_instruction fi { instr };
		auto addr = cpu.reg(fi.Itype.rs1) + fi.Itype.signed_imm();
		auto& dst = cpu.registers().getfl(fi.Itype.rd);
		dst.load_u32(cpu.machine().memory.template read<uint32_t> (addr));
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		const rv32f_instruction fi { instr };
		static const std::array<const char*, 8> insn {
			"???", "FLH", "FLW", "FLD", "FLQ", "???", "???", "???"
		};
		return snprintf(buffer, len, "%s %s, [%s%+d]",
						insn[fi.Itype.funct3],
                        RISCV::flpname(fi.Itype.rd),
                        RISCV::regname(fi.Stype.rs1),
						fi.Itype.signed_imm());
	});
	FLOAT_INSTR(FLD,
	[] (auto& cpu, rv32i_instruction instr)
	{
		const rv32f_instruction fi { instr };
		auto addr = cpu.reg(fi.Itype.rs1) + fi.Itype.signed_imm();
		auto& dst = cpu.registers().getfl(fi.Itype.rd);
		dst.load_u64(cpu.machine().memory.template read<uint64_t> (addr));
	}, DECODED_FLOAT(FLW).printer);

	FLOAT_INSTR(FSW,
	[] (auto& cpu, rv32i_instruction instr)
	{
		const rv32f_instruction fi { instr };
		auto& src = cpu.registers().getfl(fi.Stype.rs2);
		auto addr = cpu.reg(fi.Stype.rs1) + fi.Stype.signed_imm();
		cpu.machine().memory.template write<uint32_t> (addr, src.i32[0]);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		const rv32f_instruction fi { instr };
		static const std::array<const char*, 8> insn {
			"???", "FSH", "FSW", "FSD", "FSQ", "???", "???", "???"
		};
		return snprintf(buffer, len, "%s [%s%+d], %s",
						insn[fi.Stype.funct3],
                        RISCV::regname(fi.Stype.rs1),
						fi.Stype.signed_imm(),
						RISCV::flpname(fi.Stype.rs2));
	});
	FLOAT_INSTR(FSD,
	[] (auto& cpu, rv32i_instruction instr)
	{
		const rv32f_instruction fi { instr };
		auto& src = cpu.registers().getfl(fi.Stype.rs2);
		auto addr = cpu.reg(fi.Stype.rs1) + fi.Stype.signed_imm();
		cpu.machine().memory.template write<uint64_t> (addr, src.i64);
	}, DECODED_FLOAT(FSW).printer);

	FLOAT_INSTR(FMADD,
	[] (auto& cpu, rv32i_instruction instr)
	{
		const rv32f_instruction fi { instr };
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		auto& rs3 = cpu.registers().getfl(fi.R4type.rs3);
		if (fi.R4type.funct2 == 0x0) { // float32
			dst.set_float(rs1.f32[0] * rs2.f32[0] + rs3.f32[0]);
		} else if (fi.R4type.funct2 == 0x1) { // float64
			dst.f64 = rs1.f64 * rs2.f64 + rs3.f64;
		} else {
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		const rv32f_instruction fi { instr };
		static const std::array<const char*, 4> f2 {
			"FMADD.S", "FMADD.D", "???", "FMADD.Q"
		};
		return snprintf(buffer, len, "%s %s * %s + %s, %s", f2[fi.R4type.funct2],
						RISCV::flpname(fi.R4type.rs1),
						RISCV::flpname(fi.R4type.rs2),
						RISCV::flpname(fi.R4type.rs3),
						RISCV::flpname(fi.R4type.rd));
	});

	FLOAT_INSTR(FMSUB,
	[] (auto& cpu, rv32i_instruction instr)
	{
		const rv32f_instruction fi { instr };
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		auto& rs3 = cpu.registers().getfl(fi.R4type.rs3);
		if (fi.R4type.funct2 == 0x0) { // float32
			dst.set_float(rs1.f32[0] * rs2.f32[0] - rs3.f32[0]);
		} else if (fi.R4type.funct2 == 0x1) { // float64
			dst.f64 = rs1.f64 * rs2.f64 - rs3.f64;
		} else {
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		const rv32f_instruction fi { instr };
		static const std::array<const char*, 4> f2 {
			"FMSUB.S", "FMSUB.D", "???", "FMSUB.Q"
		};
		return snprintf(buffer, len, "%s %s * %s - %s, %s", f2[fi.R4type.funct2],
						RISCV::flpname(fi.R4type.rs1),
						RISCV::flpname(fi.R4type.rs2),
						RISCV::flpname(fi.R4type.rs3),
						RISCV::flpname(fi.R4type.rd));
	});

	FLOAT_INSTR(FNMADD,
	[] (auto& cpu, rv32i_instruction instr)
	{
		const rv32f_instruction fi { instr };
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		auto& rs3 = cpu.registers().getfl(fi.R4type.rs3);
		if (fi.R4type.funct2 == 0x0) { // float32
			dst.set_float(-(rs1.f32[0] * rs2.f32[0]) - rs3.f32[0]);
		} else if (fi.R4type.funct2 == 0x1) { // float64
			dst.f64 = -(rs1.f64 * rs2.f64) - rs3.f64;
		} else {
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		const rv32f_instruction fi { instr };
		static const std::array<const char*, 4> f2 {
			"FMADD.S", "FMADD.D", "???", "FMADD.Q"
		};
		return snprintf(buffer, len, "%s %s %s, %s", f2[fi.R4type.funct2],
						RISCV::flpname(fi.R4type.rs1),
						RISCV::flpname(fi.R4type.rs2),
						RISCV::flpname(fi.R4type.rd));
	});

	FLOAT_INSTR(FNMSUB,
	[] (auto& cpu, rv32i_instruction instr)
	{
		const rv32f_instruction fi { instr };
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		auto& rs3 = cpu.registers().getfl(fi.R4type.rs3);
		if (fi.R4type.funct2 == 0x0) { // float32
			dst.set_float(-(rs1.f32[0] * rs2.f32[0]) + rs3.f32[0]);
		} else if (fi.R4type.funct2 == 0x1) { // float64
			dst.f64 = -(rs1.f64 * rs2.f64) + rs3.f64;
		} else {
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		const rv32f_instruction fi { instr };
		static const std::array<const char*, 4> f2 {
			"FNMSUB.S", "FNMSUB.D", "???", "FNMSUB.Q"
		};
		return snprintf(buffer, len, "%s -(%s * %s) + %s, %s",
						f2[fi.R4type.funct2],
						RISCV::flpname(fi.R4type.rs1),
						RISCV::flpname(fi.R4type.rs2),
						RISCV::flpname(fi.R4type.rs3),
						RISCV::flpname(fi.R4type.rd));
	});

	FLOAT_INSTR(FADD,
	[] (auto& cpu, rv32i_instruction instr)
	{
		const rv32f_instruction fi { instr };
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		if (fi.R4type.funct2 == 0x0) { // float32
			dst.set_float(rs1.f32[0] + rs2.f32[0]);
		} else if (fi.R4type.funct2 == 0x1) { // float64
			dst.f64 = rs1.f64 + rs2.f64;
		} else {
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		const rv32f_instruction fi { instr };
		static const std::array<const char*, 4> f2 {
			"FADD.S", "FADD.D", "???", "FADD.Q"
		};
		return snprintf(buffer, len, "%s %s %s, %s", f2[fi.R4type.funct2],
						RISCV::flpname(fi.R4type.rs1),
						RISCV::flpname(fi.R4type.rs2),
						RISCV::flpname(fi.R4type.rd));
	});

	FLOAT_INSTR(FSUB,
	[] (auto& cpu, rv32i_instruction instr)
	{
		const rv32f_instruction fi { instr };
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		if (fi.R4type.funct2 == 0x0) { // float32
			dst.set_float(rs1.f32[0] - rs2.f32[0]);
		} else if (fi.R4type.funct2 == 0x1) { // float64
			dst.f64 = rs1.f64 - rs2.f64;
		} else {
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		const rv32f_instruction fi { instr };
		static const std::array<const char*, 4> f2 {
			"FSUB.S", "FSUB.D", "???", "FSUB.Q"
		};
		return snprintf(buffer, len, "%s %s %s, %s", f2[fi.R4type.funct2],
						RISCV::flpname(fi.R4type.rs1),
						RISCV::flpname(fi.R4type.rs2),
						RISCV::flpname(fi.R4type.rd));
	});

	FLOAT_INSTR(FMUL,
	[] (auto& cpu, rv32i_instruction instr)
	{
		const rv32f_instruction fi { instr };
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		if (fi.R4type.funct2 == 0x0) { // float32
			dst.set_float(rs1.f32[0] * rs2.f32[0]);
		} else if (fi.R4type.funct2 == 0x1) { // float64
			dst.f64 = rs1.f64 * rs2.f64;
		} else {
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		const rv32f_instruction fi { instr };
		static const std::array<const char*, 4> f2 {
			"FMUL.S", "FMUL.D", "???", "FMUL.Q"
		};
		return snprintf(buffer, len, "%s %s %s, %s", f2[fi.R4type.funct2],
						RISCV::flpname(fi.R4type.rs1),
						RISCV::flpname(fi.R4type.rs2),
						RISCV::flpname(fi.R4type.rd));
	});

	FLOAT_INSTR(FDIV,
	[] (auto& cpu, rv32i_instruction instr)
	{
		const rv32f_instruction fi { instr };
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		if (fi.R4type.funct2 == 0x0) { // fp32
			dst.set_float(rs1.f32[0] / rs2.f32[0]);
		} else if (fi.R4type.funct2 == 0x1) { // fp64
			dst.f64 = rs1.f64 / rs2.f64;
		} else {
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		const rv32f_instruction fi { instr };
		static const std::array<const char*, 4> f2 {
			"FDIV.S", "FDIV.D", "???", "FDIV.Q"
		};
		return snprintf(buffer, len, "%s %s %s, %s", f2[fi.R4type.funct2],
						RISCV::flpname(fi.R4type.rs1),
						RISCV::flpname(fi.R4type.rs2),
						RISCV::flpname(fi.R4type.rd));
	});

	FLOAT_INSTR(FSQRT,
	[] (auto& cpu, rv32i_instruction instr)
	{
		const rv32f_instruction fi { instr };
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		switch (fi.R4type.funct2) {
		case 0x0: // FSQRT.S
			dst.set_float(sqrtf(rs1.f32[0]));
			break;
		case 0x1: // FSQRT.D
			dst.f64 = sqrt(rs1.f64);
			break;
		default:
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		const rv32f_instruction fi { instr };
		static const std::array<const char*, 4> f2 {
			"FSQRT.S", "FSQRT.D", "???", "FSQRT.Q"
		};
		return snprintf(buffer, len, "%s %s, %s", f2[fi.R4type.funct2],
						RISCV::flpname(fi.R4type.rs1),
						RISCV::flpname(fi.R4type.rd));
	});

	FLOAT_INSTR(FMIN_FMAX,
	[] (auto& cpu, rv32i_instruction instr)
	{
		const rv32f_instruction fi { instr };
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		auto& dst = cpu.registers().getfl(fi.R4type.rd);

		switch (fi.R4type.funct3 | (fi.R4type.funct2 << 4))
		{
		case 0x0: // FMIN.S
			dst.set_float(std::min(rs1.f32[0], rs2.f32[0]));
			break;
		case 0x1: // FMAX.S
			dst.set_float(std::max(rs1.f32[0], rs2.f32[0]));
			break;
		case 0x10: // FMIN.D
			dst.f64 = std::min(rs1.f64, rs2.f64);
			break;
		case 0x11: // FMAX.D
			dst.f64 = std::max(rs1.f64, rs2.f64);
			break;
		default:
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		const rv32f_instruction fi { instr };
		static const std::array<const char*, 8> insn {
			"FMIN", "FMAX", "???", "???", "???", "???", "???", "???"
		};
		return snprintf(buffer, len, "%s.%c %s %s, %s",
						insn[fi.R4type.funct3],
						RISCV::flpsize(fi.R4type.funct2),
						RISCV::flpname(fi.R4type.rs1),
						RISCV::flpname(fi.R4type.rs2),
						RISCV::regname(fi.R4type.rd));
	});

	FLOAT_INSTR(FEQ_FLT_FLE,
	[] (auto& cpu, rv32i_instruction instr)
	{
		const rv32f_instruction fi { instr };
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		auto& dst = cpu.reg(fi.R4type.rd);

		switch (fi.R4type.funct3 | (fi.R4type.funct2 << 4))
		{
		case 0x0: // FLE.S
			dst = (rs1.f32[0] <= rs2.f32[0]) ? 1 : 0;
			break;
		case 0x1: // FLT.S
			dst = (rs1.f32[0] < rs2.f32[0]) ? 1 : 0;
			break;
		case 0x2: // FEQ.S
			dst = (rs1.f32[0] == rs2.f32[0]) ? 1 : 0;
			break;
		case 0x10: // FLE.D
			dst = (rs1.f64 <= rs2.f64) ? 1 : 0;
			break;
		case 0x11: // FLT.D
			dst = (rs1.f64 < rs2.f64) ? 1 : 0;
			break;
		case 0x12: // FEQ.D
			dst = (rs1.f64 == rs2.f64) ? 1 : 0;
			break;
		default:
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		const rv32f_instruction fi { instr };
		static const std::array<const char*, 4> insn {
			"FLE", "FLT", "FEQ", "F???"
		};
		return snprintf(buffer, len, "%s.%c %s %s, %s",
						insn[fi.R4type.funct3],
						RISCV::flpsize(fi.R4type.funct2),
						RISCV::flpname(fi.R4type.rs1),
						RISCV::flpname(fi.R4type.rs2),
						RISCV::regname(fi.R4type.rd));
	});

	FLOAT_INSTR(FCVT_SD_DS,
	[] (auto& cpu, rv32i_instruction instr)
	{
		const rv32f_instruction fi { instr };
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		switch (fi.R4type.funct2) {
		case 0x0: // FCVT.S.D (64 -> 32)
			dst.set_float(rs1.f64);
			break;
		case 0x1: // FCVT.D.S (32 -> 64)
			dst.f64 = rs1.f32[0];
			break;
		default:
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		const rv32f_instruction fi { instr };
		static const std::array<const char*, 4> f2 {
			"FCVT.S.D", "FCVT.D.S", "???", "???"
		};
		return snprintf(buffer, len, "%s %s, %s", f2[fi.R4type.funct2],
						RISCV::flpname(fi.R4type.rs1),
						RISCV::flpname(fi.R4type.rd));
	});

	FLOAT_INSTR(FCVT_W_SD,
	[] (auto& cpu, rv32i_instruction instr)
	{
		const rv32f_instruction fi { instr };
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& dst = cpu.reg(fi.R4type.rd);
		switch (fi.R4type.funct2) {
		case 0x0: // from float32
			if (fi.R4type.rs2 == 0x0)
				dst = (int32_t) rs1.f32[0];
			else
				dst = (uint32_t) rs1.f32[0];
			break;
		case 0x1: // from float64
			if (fi.R4type.rs2 == 0x0)
				dst = (int32_t) rs1.f64;
			else
				dst = (uint32_t) rs1.f64;
			break;
		default:
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		const rv32f_instruction fi { instr };
		static const std::array<const char*, 4> f2 {
			"FCVT.W.S", "FCVT.W.D", "???", "FCVT.W.Q"
		};
		return snprintf(buffer, len, "%s %s, %s", f2[fi.R4type.funct2],
						RISCV::flpname(fi.R4type.rs1),
						RISCV::regname(fi.R4type.rd));
	});

	FLOAT_INSTR(FCVT_SD_W,
	[] (auto& cpu, rv32i_instruction instr)
	{
		const rv32f_instruction fi { instr };
		auto& rs1 = cpu.reg(fi.R4type.rs1);
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		switch (fi.R4type.funct2) {
		case 0x0: // to float32
			if (fi.R4type.rs2 == 0x0)
				dst.set_float((RVSIGNTYPE(cpu)) rs1);
			else
				dst.set_float(rs1);
			break;
		case 0x1: // to float64
			if (fi.R4type.rs2 == 0x0) {
				dst.f64 = (RVSIGNTYPE(cpu)) rs1;
			} else
				dst.f64 = rs1;
			break;
		default:
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		const rv32f_instruction fi { instr };
		static const std::array<const char*, 4> f2 {
			"FCVT.S.W", "FCVT.D.W", "???", "FCVT.Q.W"
		};
		return snprintf(buffer, len, "%s %s, %s", f2[fi.R4type.funct2],
						RISCV::regname(fi.R4type.rs1),
						RISCV::flpname(fi.R4type.rd));
	});

	FLOAT_INSTR(FSGNJ_NX,
	[] (auto& cpu, rv32i_instruction instr)
	{
		const rv32f_instruction fi { instr };
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		switch (fi.R4type.funct3) {
		case 0x0: // FSGNJ
			switch (fi.R4type.funct2) {
			case 0x0: // float32
				dst.load_u32((rs2.lsign.sign << 31) | rs1.lsign.bits);
				break;
			case 0x1: // float64
				dst.i64 = ((uint64_t) rs2.usign.sign << 63) | rs1.usign.bits;
				break;
			default:
				cpu.trigger_exception(ILLEGAL_OPERATION);
			}
			break;
		case 0x1: // FSGNJ_N
			switch (fi.R4type.funct2) {
			case 0x0: // float32
				dst.load_u32((~rs2.lsign.sign << 31) | rs1.lsign.bits);
				break;
			case 0x1: // float64
				dst.i64 = (~(uint64_t) rs2.usign.sign << 63) | rs1.usign.bits;
				break;
			default:
				cpu.trigger_exception(ILLEGAL_OPERATION);
			}
			break;
		case 0x2: // FSGNJ_X
			switch (fi.R4type.funct2) {
			case 0x0: // float32
				dst.load_u32(((rs1.lsign.sign ^ rs2.lsign.sign) << 31) | rs1.lsign.bits);
				break;
			case 0x1: // float64
				dst.i64 = ((uint64_t)(rs1.usign.sign ^ rs2.usign.sign) << 63) | rs1.usign.bits;
				break;
			default:
				cpu.trigger_exception(ILLEGAL_OPERATION);
			}
			break;
		default:
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		const rv32f_instruction fi { instr };

		if (fi.R4type.rs1 == fi.R4type.rs2) {
			static const char* insn[4] = {"FMV", "FNEG", "FABS", "???"};
			return snprintf(buffer, len, "%s.%c %s, %s",
							insn[fi.R4type.funct3],
							RISCV::flpsize(fi.R4type.funct2),
							RISCV::flpname(fi.R4type.rs1),
							RISCV::flpname(fi.R4type.rd));
		}
		static const char* insn[4] = {"FSGNJ", "FSGNJN", "FSGNJX", "???"};
		return snprintf(buffer, len, "%s.%c %s %s, %s",
						insn[fi.R4type.funct3],
						RISCV::flpsize(fi.R4type.funct2),
						RISCV::flpname(fi.R4type.rs1),
						RISCV::flpname(fi.R4type.rs2),
						RISCV::flpname(fi.R4type.rd));
	});

	FLOAT_INSTR(FMV_X_W, // 1110
	[] (auto& cpu, rv32i_instruction instr)
	{
		const rv32f_instruction fi { instr };
		auto& dst = cpu.reg(fi.R4type.rd);
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		switch (fi.R4type.funct2) {
		case 0x0: // FMV.X.W
			dst = rs1.i32[0];
			break;
		case 0x1: // FMV.X.D
			if constexpr (RVIS64BIT(cpu)) {
				dst = rs1.i64;
				break;
			} else {
				cpu.trigger_exception(ILLEGAL_OPERATION);
			}
		default:
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		const rv32f_instruction fi { instr };
		static const std::array<const char*, 4> f2 {
			"FMV.X.W", "FMV.X.D", "???", "FMV.X.Q"
		};
		return snprintf(buffer, len, "%s %s, %s", f2[fi.R4type.funct2],
						RISCV::flpname(fi.R4type.rs1),
						RISCV::regname(fi.R4type.rd));
	});

	FLOAT_INSTR(FMV_W_X, // 1111
	[] (auto& cpu, rv32i_instruction instr)
	{
		const rv32f_instruction fi { instr };
		auto& rs1 = cpu.reg(fi.R4type.rs1);
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		switch (fi.R4type.funct2) {
		case 0x0: // FMV.W.X
			dst.load_u32(rs1);
			break;
		case 0x1: // FMV.D.X
			if constexpr (RVIS64BIT(cpu)) {
				dst.load_u64(rs1);
				break;
			} else {
				cpu.trigger_exception(ILLEGAL_OPERATION);
			}
		default:
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		const rv32f_instruction fi { instr };
		static const std::array<const char*, 4> f2 {
			"FMV.W.X", "FMV.D.X", "???", "FMV.Q.X"
		};
		return snprintf(buffer, len, "%s %s, %s", f2[fi.R4type.funct2],
						RISCV::regname(fi.R4type.rs1),
						RISCV::flpname(fi.R4type.rd));
	});
}
