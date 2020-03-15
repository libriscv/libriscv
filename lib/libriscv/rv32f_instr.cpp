#include "rvfd.hpp"
#include "instr_helpers.hpp"

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
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
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
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
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

	FLOAT_INSTR(FMADD,
	[] (auto& cpu, rv32i_instruction instr)
	{
		rv32f_instruction fi { instr };
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		auto& rs3 = cpu.registers().getfl(fi.R4type.rs3);
		if (fi.R4type.funct2 == 0x0) { // float32
			dst.f32[0] = rs1.f32[0] * rs2.f32[0] + rs3.f32[0];
			dst.nanbox();
			return;
		} else if (fi.R4type.funct2 == 0x1) { // float64
			dst.f64 = rs1.f64 * rs2.f64 + rs3.f64;
			return;
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		rv32f_instruction fi { instr };
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
		rv32f_instruction fi { instr };
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		auto& rs3 = cpu.registers().getfl(fi.R4type.rs3);
		if (fi.R4type.funct2 == 0x0) { // float32
			dst.f32[0] = rs1.f32[0] * rs2.f32[0] - rs3.f32[0];
			dst.nanbox();
			return;
		} else if (fi.R4type.funct2 == 0x1) { // float64
			dst.f64 = rs1.f64 * rs2.f64 - rs3.f64;
			return;
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		rv32f_instruction fi { instr };
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
		rv32f_instruction fi { instr };
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		auto& rs3 = cpu.registers().getfl(fi.R4type.rs3);
		if (fi.R4type.funct2 == 0x0) { // float32
			dst.f32[0] = -(rs1.f32[0] * rs2.f32[0]) - rs3.f32[0];
			dst.nanbox();
			return;
		} else if (fi.R4type.funct2 == 0x1) { // float64
			dst.f64 = -(rs1.f64 * rs2.f64) - rs3.f64;
			return;
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		rv32f_instruction fi { instr };
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
		rv32f_instruction fi { instr };
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		auto& rs3 = cpu.registers().getfl(fi.R4type.rs3);
		if (fi.R4type.funct2 == 0x0) { // float32
			dst.f32[0] = -(rs1.f32[0] * rs2.f32[0]) - rs3.f32[0];
			dst.nanbox();
			return;
		} else if (fi.R4type.funct2 == 0x1) { // float64
			dst.f64 = -(rs1.f64 * rs2.f64) + rs3.f64;
			return;
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		rv32f_instruction fi { instr };
		static const std::array<const char*, 4> f2 {
			"FNMSUB.S", "FNMSUB.D", "???", "FNMSUB.Q"
		};
		return snprintf(buffer, len, "%s %s %s, %s", f2[fi.R4type.funct2],
						RISCV::flpname(fi.R4type.rs1),
						RISCV::flpname(fi.R4type.rs2),
						RISCV::flpname(fi.R4type.rd));
	});

	FLOAT_INSTR(FADD,
	[] (auto& cpu, rv32i_instruction instr)
	{
		rv32f_instruction fi { instr };
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		if (fi.R4type.funct2 == 0x0) { // float32
			dst.f32[0] = rs1.f32[0] + rs2.f32[0];
			dst.nanbox();
			return;
		} else if (fi.R4type.funct2 == 0x1) { // float64
			dst.f64 = rs1.f64 + rs2.f64;
			return;
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		rv32f_instruction fi { instr };
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
		rv32f_instruction fi { instr };
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		if (fi.R4type.funct2 == 0x0) { // float32
			dst.f32[0] = rs1.f32[0] - rs2.f32[0];
			dst.nanbox();
			return;
		} else if (fi.R4type.funct2 == 0x1) { // float64
			dst.f64 = rs1.f64 - rs2.f64;
			return;
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		rv32f_instruction fi { instr };
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
		rv32f_instruction fi { instr };
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		if (fi.R4type.funct2 == 0x0) { // float32
			dst.f32[0] = rs1.f32[0] * rs2.f32[0];
			dst.nanbox();
			return;
		} else if (fi.R4type.funct2 == 0x1) { // float64
			dst.f64 = rs1.f64 * rs2.f64;
			return;
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		rv32f_instruction fi { instr };
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
		rv32f_instruction fi { instr };
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		if (fi.R4type.funct2 == 0x0) { // float32
			dst.f32[0] = rs1.f32[0] / rs2.f32[0];
			dst.nanbox();
			return;
		} else if (fi.R4type.funct2 == 0x1) { // float64
			dst.f64 = rs1.f64 / rs2.f64;
			return;
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		rv32f_instruction fi { instr };
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
		rv32f_instruction fi { instr };
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		switch (fi.R4type.funct2) {
			case 0x0: // FSQRT.S
				dst.set_float(sqrtf(rs1.f32[0]));
				return;
			case 0x1: // FSQRT.D
				dst.f64 = sqrt(rs1.f64);
				return;
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		rv32f_instruction fi { instr };
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
		rv32f_instruction fi { instr };
		if (fi.R4type.rd != 0)
		{
			auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
			auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
			auto& dst = cpu.registers().getfl(fi.R4type.rd);

			switch (fi.R4type.funct3 | (fi.R4type.funct2 << 4))
			{
				case 0x0: // FMIN.S
					dst.f32[0] = std::min(rs1.f32[0], rs2.f32[0]);
					dst.nanbox();
					return;
				case 0x1: // FMAX.S
					dst.f32[0] = std::max(rs1.f32[0], rs2.f32[0]);
					dst.nanbox();
					return;
				case 0x10: // FMIN.D
					dst.f64 = std::min(rs1.f64, rs2.f64);
					return;
				case 0x11: // FMAX.D
					dst.f64 = std::max(rs1.f64, rs2.f64);
					return;
			}
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		rv32f_instruction fi { instr };
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
		rv32f_instruction fi { instr };
		if (fi.R4type.rd != 0)
		{
			auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
			auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
			auto& dst = cpu.reg(fi.R4type.rd);

			switch (fi.R4type.funct3 | (fi.R4type.funct2 << 4))
			{
				case 0x0: // FLE.S
					dst = (rs1.f32[0] <= rs2.f32[0]) ? 1 : 0;
					return;
				case 0x1: // FLT.S
					dst = (rs1.f32[0] < rs2.f32[0]) ? 1 : 0;
					return;
				case 0x2: // FEQ.S
					dst = (rs1.f32[0] == rs2.f32[0]) ? 1 : 0;
					return;
				case 0x10: // FLE.D
					dst = (rs1.f64 <= rs2.f64) ? 1 : 0;
					return;
				case 0x11: // FLT.D
					dst = (rs1.f64 < rs2.f64) ? 1 : 0;
					return;
				case 0x12: // FEQ.D
					dst = (rs1.f64 == rs2.f64) ? 1 : 0;
					return;
			}
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		rv32f_instruction fi { instr };
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
		rv32f_instruction fi { instr };
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		switch (fi.R4type.funct2) {
			case 0x0: // FCVT.S.D (64 -> 32)
				dst.f32[0] = rs1.f64;
				dst.nanbox();
				return;
			case 0x1: // FCVT.D.S (32 -> 64)
				dst.f64 = rs1.f32[0];
				return;
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		rv32f_instruction fi { instr };
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
		rv32f_instruction fi { instr };
		if (fi.R4type.rd != 0)
		{
			auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
			auto& dst = cpu.reg(fi.R4type.rd);
			switch (fi.R4type.funct2) {
				case 0x0: // from float32
					if (fi.R4type.rs2 == 0x0)
						dst = (int32_t) rs1.f32[0];
					else
						dst = (uint32_t) rs1.f32[0];
					return;
				case 0x1: // from float64
					if (fi.R4type.rs2 == 0x0)
						dst = (int32_t) rs1.f64;
					else
						dst = (uint32_t) rs1.f64;
					return;
			}
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		rv32f_instruction fi { instr };
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
		rv32f_instruction fi { instr };
		auto& rs1 = cpu.reg(fi.R4type.rs1);
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		switch (fi.R4type.funct2) {
			case 0x0: // to float32
				dst.f32[0] = rs1;
				dst.nanbox();
				return;
			case 0x1: // to float64
				dst.f64 = rs1;
				return;
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		rv32f_instruction fi { instr };
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
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		rv32f_instruction fi { instr };

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
		rv32f_instruction fi { instr };
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& dst = cpu.reg(fi.R4type.rd);
		switch (fi.R4type.funct2) {
			case 0x0: // FMV.X.W
				dst = rs1.i32[0];
				return;
			case 0x1: // FMV.X.D
				cpu.trigger_exception(ILLEGAL_OPERATION);
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		rv32f_instruction fi { instr };
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
		rv32f_instruction fi { instr };
		auto& rs1 = cpu.reg(fi.R4type.rs1);
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		switch (fi.R4type.funct2) {
			case 0x0: // FMV.W.X
				dst.set_float(rs1);
				return;
			case 0x1: // FMV.D.X
				cpu.trigger_exception(ILLEGAL_OPERATION);
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		rv32f_instruction fi { instr };
		static const std::array<const char*, 4> f2 {
			"FMV.W.X", "FMV.D.X", "???", "FMV.Q.X"
		};
		return snprintf(buffer, len, "%s %s, %s", f2[fi.R4type.funct2],
						RISCV::regname(fi.R4type.rs1),
						RISCV::flpname(fi.R4type.rd));
	});
}
