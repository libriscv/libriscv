#include "rvfd.hpp"
#include "instr_helpers.hpp"
#include "rvfd_util.hpp"
#include <cmath>
#include <limits>
#include <type_traits>

namespace riscv
{
	// The canonical NaNs, the rounding modes, fcvt_to_integer() and the FMIN/
	// FMAX and FCLASS rules all live in rvfd_util.hpp, because the asmjit
	// backend's host helpers must implement exactly the same semantics.

	// Convert an integer to float/double, reporting NX when the destination
	// mantissa was too narrow to hold it. long double is exact for every 64-bit
	// integer wherever it is an 80- or 128-bit format; where it is merely double
	// (MSVC), the 64-bit sources simply do not report NX. Only compiled in under
	// FCSR emulation — the comparison is not free.
	template <typename F, typename T>
	static inline F fcvt_from_integer(T value, bool& inexact) {
		const F converted = F(value);
		if constexpr (fcsr_emulation)
			inexact = (long double)converted != (long double)value;
		return converted;
	}

	// A signaling NaN has an all-ones exponent, a clear quiet bit and a non-zero
	// payload. The payload check is what separates it from an infinity, and the
	// quiet bit must be masked at bit 22 (f32) / bit 51 (f64), not one below.
	template <typename T>
	static bool is_signaling_nan(T t) {
		if constexpr (sizeof(T) == 4) {
			uint32_t bits;
			__builtin_memcpy(&bits, &t, 4);
			return (bits & 0x7fc00000) == 0x7f800000 && (bits & 0x003fffff) != 0;
		} else {
			uint64_t bits;
			__builtin_memcpy(&bits, &t, 8);
			return (bits & 0x7ff8000000000000ull) == 0x7ff0000000000000ull
				&& (bits & 0x0007ffffffffffffull) != 0;
		}
	}

#ifdef RISCV_FCSR
	template <int W, typename T>
	static void fsflags(CPU<W>& cpu, long double exact, T& inexact) {
		if constexpr (fcsr_emulation) {
			auto& fcsr = cpu.registers().fcsr();
			fcsr.fflags = 0;
			if (std::isnan(exact) || std::isnan(inexact)) {
				fcsr.fflags |= 16;
				// Canonical NaN
				if constexpr (sizeof(T) == 4)
					*(int32_t *)&inexact = CANONICAL_NAN_F32;
				else
					*(int64_t *)&inexact = CANONICAL_NAN_F64;
			} else {
				if (exact != inexact) fcsr.fflags |= 1;
			}
		}
	}
#else
#define fsflags(c, e, i) /**/
#endif
	template <bool Signaling, int W, typename T, typename R>
	static void feqflags(CPU<W>& cpu, T a, T b, R& dst) {
		if constexpr (fcsr_emulation) {
			auto& fcsr = cpu.registers().fcsr();
			fcsr.fflags = 0;
			if (std::isnan(a) || std::isnan(b)) {
				// All operations return 0 when either operand is NaN
				dst = 0;
			}
			if constexpr (Signaling) {
				if (std::isnan(a) || std::isnan(b))
					fcsr.fflags |= 16;
			} else { // Quiet
				if (is_signaling_nan(a) || is_signaling_nan(b))
					fcsr.fflags |= 16;
			}
		}
	}

	FLOAT_INSTR(FLW,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		auto addr = cpu.reg(fi.Itype.rs1) + fi.Itype.signed_imm();
		auto& dst = cpu.registers().getfl(fi.Itype.rd);
		dst.load_u32(cpu.machine().memory.template read<uint32_t> (addr));
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
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
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		auto addr = cpu.reg(fi.Itype.rs1) + fi.Itype.signed_imm();
		auto& dst = cpu.registers().getfl(fi.Itype.rd);
		dst.load_u64(cpu.machine().memory.template read<uint64_t> (addr));
	}, DECODED_FLOAT(FLW).printer);

	FLOAT_INSTR(FSW,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		const auto& src = cpu.registers().getfl(fi.Stype.rs2);
		auto addr = cpu.reg(fi.Stype.rs1) + fi.Stype.signed_imm();
		cpu.machine().memory.template write<uint32_t> (addr, src.i32[0]);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
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
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		const auto& src = cpu.registers().getfl(fi.Stype.rs2);
		auto addr = cpu.reg(fi.Stype.rs1) + fi.Stype.signed_imm();
		cpu.machine().memory.template write<uint64_t> (addr, src.i64);
	}, DECODED_FLOAT(FSW).printer);

	FLOAT_INSTR(FMADD,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		auto& rs3 = cpu.registers().getfl(fi.R4type.rs3);
		// RISC-V spec §11.6: FMA must round only once (fused).
		// std::fma is IEEE 754 fused multiply-add.
		if (fi.R4type.funct2 == 0x0) { // float32
			dst.set_float(std::fma(rs1.f32[0], rs2.f32[0], rs3.f32[0]));
			fsflags(cpu, (double)rs1.f32[0] * (double)rs2.f32[0] + (double)rs3.f32[0], dst.f32[0]);
		} else if (fi.R4type.funct2 == 0x1) { // float64
			dst.f64 = std::fma(rs1.f64, rs2.f64, rs3.f64);
			fsflags(cpu, (long double)rs1.f64 * (long double)rs2.f64 + (long double)rs3.f64, dst.f64);
		} else {
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
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
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		auto& rs3 = cpu.registers().getfl(fi.R4type.rs3);
		// RISC-V spec §11.6: FMA must round only once (fused).
		// FMSUB = rs1*rs2 - rs3 = fma(rs1, rs2, -rs3).
		if (fi.R4type.funct2 == 0x0) { // float32
			dst.set_float(std::fma(rs1.f32[0], rs2.f32[0], -rs3.f32[0]));
			fsflags(cpu, (double)rs1.f32[0] * (double)rs2.f32[0] - (double)rs3.f32[0], dst.f32[0]);
		} else if (fi.R4type.funct2 == 0x1) { // float64
			dst.f64 = std::fma(rs1.f64, rs2.f64, -rs3.f64);
			fsflags(cpu, (long double)rs1.f64 * (long double)rs2.f64 - (long double)rs3.f64, dst.f64);
		} else {
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
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
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		auto& rs3 = cpu.registers().getfl(fi.R4type.rs3);
		// RISC-V spec §11.6: FMA must round only once (fused).
		// FNMADD = -(rs1*rs2) - rs3 = -fma(rs1, rs2, rs3).
		if (fi.R4type.funct2 == 0x0) { // float32
			dst.set_float(-std::fma(rs1.f32[0], rs2.f32[0], rs3.f32[0]));
			fsflags(cpu, (double)-rs1.f32[0] * (double)rs2.f32[0] - (double)rs3.f32[0], dst.f32[0]);
		} else if (fi.R4type.funct2 == 0x1) { // float64
			dst.f64 = -std::fma(rs1.f64, rs2.f64, rs3.f64);
			fsflags(cpu, (long double)-rs1.f64 * (long double)rs2.f64 - (long double)rs3.f64, dst.f64);
		} else {
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
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
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		auto& rs3 = cpu.registers().getfl(fi.R4type.rs3);
		// RISC-V spec §11.6: FMA must round only once (fused).
		// FNMSUB = -(rs1*rs2) + rs3 = fma(-rs1, rs2, rs3).
		if (fi.R4type.funct2 == 0x0) { // float32
			dst.set_float(std::fma(-rs1.f32[0], rs2.f32[0], rs3.f32[0]));
			fsflags(cpu, (double)-rs1.f32[0] * (double)rs2.f32[0] + (double)rs3.f32[0], dst.f32[0]);
		} else if (fi.R4type.funct2 == 0x1) { // float64
			dst.f64 = std::fma(-rs1.f64, rs2.f64, rs3.f64);
			fsflags(cpu, (long double)-rs1.f64 * (long double)rs2.f64 + (long double)rs3.f64, dst.f64);
		} else {
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
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
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		if (fi.R4type.funct2 == 0x0) { // float32
			// A quiet NaN operand propagates as the canonical qNaN without
			// raising NV — only a signaling NaN, or an invalid operation such
			// as inf + -inf, is invalid. fsflags() cannot tell those apart
			// because it only sees the result, so handle it up front.
			if constexpr (fcsr_emulation) {
				if ((std::isnan(rs1.f32[0]) || std::isnan(rs2.f32[0]))
					&& !is_signaling_nan(rs1.f32[0]) && !is_signaling_nan(rs2.f32[0])) {
					dst.load_u32(CANONICAL_NAN_F32);
					cpu.registers().fcsr().fflags = 0;
					return;
				}
			}
			dst.set_float(rs1.f32[0] + rs2.f32[0]);
			fsflags(cpu, (double)(rs1.f32[0]) + (double)(rs2.f32[0]), dst.f32[0]);
		} else if (fi.R4type.funct2 == 0x1) { // float64
			dst.f64 = rs1.f64 + rs2.f64;
			fsflags(cpu, (long double)(rs1.f64) + (long double)(rs2.f64), dst.f64);
		} else {
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
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
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		if (fi.R4type.funct2 == 0x0) { // float32
			dst.set_float(rs1.f32[0] - rs2.f32[0]);
			fsflags(cpu, (double)(rs1.f32[0]) - (double)(rs2.f32[0]), dst.f32[0]);
		} else if (fi.R4type.funct2 == 0x1) { // float64
			dst.f64 = rs1.f64 - rs2.f64;
			fsflags(cpu, (long double)(rs1.f64) - (long double)(rs2.f64), dst.f64);
		} else {
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
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
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		if (fi.R4type.funct2 == 0x0) { // float32
			// Operands are read out before the store, because rd is allowed to
			// alias rs1 or rs2.
			const uint32_t ia = rs1.i32[0], ib = rs2.i32[0];
			const float fa = rs1.f32[0], fb = rs2.f32[0];
			dst.set_float(fa * fb);
			fsflags(cpu, (double)fa * (double)fb, dst.f32[0]);
			if constexpr (fcsr_emulation) {
				// Finite inputs that produce an infinity overflowed; finite
				// inputs that produce an inexact subnormal underflowed.
				// fsflags() already raised NX for both.
				if ((ia & 0x7f800000u) != 0x7f800000u
					&& (ib & 0x7f800000u) != 0x7f800000u) {
					const uint32_t result = dst.i32[0] & 0x7fffffffu;
					if (result == 0x7f800000u)
						cpu.registers().fcsr().fflags |= 4; // OF
					else if (result < 0x00800000u
						&& (double)fa * (double)fb != dst.f32[0])
						cpu.registers().fcsr().fflags |= 2; // UF
				}
			}
		} else if (fi.R4type.funct2 == 0x1) { // float64
			dst.f64 = rs1.f64 * rs2.f64;
			fsflags(cpu, (long double)(rs1.f64) * (long double)(rs2.f64), dst.f64);
		} else {
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
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
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		if (fi.R4type.funct2 == 0x0) { // fp32
			// Operands read out before the store: rd may alias rs1 or rs2. DZ
			// is only for a finite non-zero numerator over zero: 0/0 is NV and
			// inf/0 is exact.
			const uint32_t ia = rs1.i32[0], ib = rs2.i32[0];
			const float fa = rs1.f32[0], fb = rs2.f32[0];
			dst.set_float(fa / fb);
			fsflags(cpu, (double)fa / (double)fb, dst.f32[0]);
			if constexpr (fcsr_emulation) {
				if ((ia & 0x7fffffffu) != 0 && (ia & 0x7f800000u) != 0x7f800000u
					&& (ib & 0x7fffffffu) == 0)
					cpu.registers().fcsr().fflags |= 8; // DZ
			}
		} else if (fi.R4type.funct2 == 0x1) { // fp64
			const uint64_t ia = rs1.i64, ib = rs2.i64;
			const double da = rs1.f64, db = rs2.f64;
			dst.f64 = da / db;
			fsflags(cpu, (long double)da / (long double)db, dst.f64);
			if constexpr (fcsr_emulation) {
				if ((ia & 0x7fffffffffffffffull) != 0
					&& (ia & 0x7ff0000000000000ull) != 0x7ff0000000000000ull
					&& (ib & 0x7fffffffffffffffull) == 0)
					cpu.registers().fcsr().fflags |= 8; // DZ
			}
		} else {
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
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
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		switch (fi.R4type.funct2) {
		case 0x0: // FSQRT.S
			// sqrt(qNaN) is the canonical qNaN and is *not* an invalid
			// operation; only a signaling NaN input raises NV. fsflags() would
			// raise it for both, so quiet NaNs are handled before we get there.
			if constexpr (fcsr_emulation) {
				if (UNLIKELY(std::isnan(rs1.f32[0]))) {
					// Classified before the store, as rd may alias rs1.
					const bool snan = is_signaling_nan(rs1.f32[0]);
					dst.load_u32(CANONICAL_NAN_F32);
					cpu.registers().fcsr().fflags = snan ? 16 : 0;
					return;
				}
			}
			dst.set_float(sqrtf(rs1.f32[0]));
			fsflags(cpu, std::sqrt((double)(rs1.f32[0])), dst.f32[0]);
			break;
		case 0x1: // FSQRT.D
			if constexpr (fcsr_emulation) {
				if (UNLIKELY(std::isnan(rs1.f64))) {
					const bool snan = is_signaling_nan(rs1.f64);
					dst.load_u64(CANONICAL_NAN_F64);
					cpu.registers().fcsr().fflags = snan ? 16 : 0;
					return;
				}
			}
			dst.f64 = sqrt(rs1.f64);
			fsflags(cpu, std::sqrt((long double)(rs1.f64)), dst.f64);
			break;
		default:
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		const rv32f_instruction fi { instr };
		static const std::array<const char*, 4> f2 {
			"FSQRT.S", "FSQRT.D", "???", "FSQRT.Q"
		};
		return snprintf(buffer, len, "%s %s, %s", f2[fi.R4type.funct2],
						RISCV::flpname(fi.R4type.rs1),
						RISCV::flpname(fi.R4type.rd));
	});

	FLOAT_INSTR(FMIN_FMAX,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_COLDATTR
	{
		const rv32f_instruction fi { instr };
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		auto& dst = cpu.registers().getfl(fi.R4type.rd);

		// RISC-V spec §11.6 FMIN/FMAX: treat -0.0 < +0.0 (IEEE 754
		// fmin/fmax leave ±0 ordering implementation-defined). The rules
		// live in rvfd_util.hpp; these wrappers only move bits in and out,
		// as an f-register holds a raw pattern rather than a host float.
		// rv_fmin/rv_fmax canonicalize two-NaN operands themselves, which
		// the non-FCSR paths below deliberately keep, as it is the only
		// result a program can meaningfully use.
		auto fmin32 = [](uint32_t ab, uint32_t bb) -> uint32_t {
			float a, b; __builtin_memcpy(&a, &ab, 4); __builtin_memcpy(&b, &bb, 4);
			float r = rv_fmin32(a, b); uint32_t rb; __builtin_memcpy(&rb, &r, 4); return rb;
		};
		auto fmax32 = [](uint32_t ab, uint32_t bb) -> uint32_t {
			float a, b; __builtin_memcpy(&a, &ab, 4); __builtin_memcpy(&b, &bb, 4);
			float r = rv_fmax32(a, b); uint32_t rb; __builtin_memcpy(&rb, &r, 4); return rb;
		};
		auto fmin64 = [](uint64_t ab, uint64_t bb) -> uint64_t {
			double a, b; __builtin_memcpy(&a, &ab, 8); __builtin_memcpy(&b, &bb, 8);
			double r = rv_fmin64(a, b); uint64_t rb; __builtin_memcpy(&rb, &r, 8); return rb;
		};
		auto fmax64 = [](uint64_t ab, uint64_t bb) -> uint64_t {
			double a, b; __builtin_memcpy(&a, &ab, 8); __builtin_memcpy(&b, &bb, 8);
			double r = rv_fmax64(a, b); uint64_t rb; __builtin_memcpy(&rb, &r, 8); return rb;
		};

		// A signaling NaN operand raises NV; a quiet one does not. Classified
		// before the result is stored, because rd may alias rs1 or rs2, and by
		// funct2 (the operand precision) rather than through the low 32 bits.
		const bool snan = !fcsr_emulation ? false : (fi.R4type.funct2 == 0x0)
			? (is_signaling_nan(rs1.f32[0]) || is_signaling_nan(rs2.f32[0]))
			: (is_signaling_nan(rs1.f64) || is_signaling_nan(rs2.f64));

		switch (fi.R4type.funct3 | (fi.R4type.funct2 << 4))
		{
		case 0x0: // FMIN.S
			if constexpr (fcsr_emulation) {
				if (std::isnan(rs1.f32[0]) && std::isnan(rs2.f32[0]))
					dst.load_u32(CANONICAL_NAN_F32);
				else if (std::isnan(rs1.f32[0]))
					dst.load_u32(rs2.i32[0]);
				else if (std::isnan(rs2.f32[0]))
					dst.load_u32(rs1.i32[0]);
				else
					dst.load_u32(fmin32(rs1.i32[0], rs2.i32[0]));
			} else {
				dst.load_u32(fmin32(rs1.i32[0], rs2.i32[0]));
			}
			break;
		case 0x1: // FMAX.S
			if constexpr (fcsr_emulation) {
				if (std::isnan(rs1.f32[0]) && std::isnan(rs2.f32[0]))
					dst.load_u32(CANONICAL_NAN_F32);
				else if (std::isnan(rs1.f32[0]))
					dst.load_u32(rs2.i32[0]);
				else if (std::isnan(rs2.f32[0]))
					dst.load_u32(rs1.i32[0]);
				else
					dst.load_u32(fmax32(rs1.i32[0], rs2.i32[0]));
			} else {
				dst.load_u32(fmax32(rs1.i32[0], rs2.i32[0]));
			}
			break;
		case 0x10: // FMIN.D
			if constexpr (fcsr_emulation) {
				if (std::isnan(rs1.f64) && std::isnan(rs2.f64))
					dst.load_u64(CANONICAL_NAN_F64);
				else
					dst.load_u64(fmin64(rs1.i64, rs2.i64));
			} else {
				dst.load_u64(fmin64(rs1.i64, rs2.i64));
			}
			break;
		case 0x11: // FMAX.D
			if constexpr (fcsr_emulation) {
				if (std::isnan(rs1.f64) && std::isnan(rs2.f64))
					dst.load_u64(CANONICAL_NAN_F64);
				else
					dst.load_u64(fmax64(rs1.i64, rs2.i64));
			} else {
				dst.load_u64(fmax64(rs1.i64, rs2.i64));
			}
			break;
		default:
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
		if constexpr (fcsr_emulation) {
			cpu.registers().fcsr().fflags = snan ? 16 : 0;
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
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
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		auto& dst = cpu.reg(fi.R4type.rd);

		switch (fi.R4type.funct3 | (fi.R4type.funct2 << 4))
		{
		case 0x0: // FLE.S
			dst = (rs1.f32[0] <= rs2.f32[0]) ? 1 : 0;
			feqflags<true>(cpu, rs1.f32[0], rs2.f32[0], dst);
			break;
		case 0x1: // FLT.S
			dst = (rs1.f32[0] < rs2.f32[0]) ? 1 : 0;
			feqflags<true>(cpu, rs1.f32[0], rs2.f32[0], dst);
			break;
		case 0x2: // FEQ.S
			dst = (rs1.f32[0] == rs2.f32[0]) ? 1 : 0;
			feqflags<false>(cpu, rs1.f32[0], rs2.f32[0], dst);
			break;
		case 0x10: // FLE.D
			dst = (rs1.f64 <= rs2.f64) ? 1 : 0;
			feqflags<true>(cpu, rs1.f64, rs2.f64, dst);
			break;
		case 0x11: // FLT.D
			dst = (rs1.f64 < rs2.f64) ? 1 : 0;
			feqflags<true>(cpu, rs1.f64, rs2.f64, dst);
			break;
		case 0x12: // FEQ.D
			dst = (rs1.f64 == rs2.f64) ? 1 : 0;
			feqflags<false>(cpu, rs1.f64, rs2.f64, dst);
			break;
		default:
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
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
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		switch (fi.R4type.funct2) {
		case 0x0: // FCVT.S.D (64 -> 32)
			if (std::isnan(rs1.f64))
				dst.load_u32(CANONICAL_NAN_F32);
			else
				dst.set_float(rs1.f64);
			break;
		case 0x1: // FCVT.D.S (32 -> 64)
			if (std::isnan(rs1.f32[0]))
				dst.load_u64(CANONICAL_NAN_F64);
			else
				dst.f64 = rs1.f32[0];
			break;
		default:
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		const rv32f_instruction fi { instr };
		static const std::array<const char*, 4> f2 {
			"FCVT.S.D", "FCVT.D.S", "???", "???"
		};
		return snprintf(buffer, len, "%s %s, %s", f2[fi.R4type.funct2],
						RISCV::flpname(fi.R4type.rs1),
						RISCV::flpname(fi.R4type.rd));
	});

	FLOAT_INSTR(FCVT_W_SD,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto rmm = fi.R4type.funct3; // rounding mode is encoded in funct3
		if (rmm == 0x7) // DYN: use the dynamic mode from the fcsr CSR (frm field)
			rmm = cpu.registers().fcsr().frm;
		auto& dst = cpu.reg(fi.R4type.rd);
		bool invalid = false;
		switch (fi.R4type.funct2) {
		case 0x0: // from float32
			switch (fi.R4type.rs2) {
			case 0x0: // FCVT.W.S (sign-extended 32-bit result)
				dst = fcvt_to_integer<int32_t>(rs1.f32[0], rmm, invalid);
				break;
			case 0x1: // FCVT.WU.S (sign-extended 32-bit result)
				dst = int32_t(fcvt_to_integer<uint32_t>(rs1.f32[0], rmm, invalid));
				break;
			case 0x2: // FCVT.L.S
				dst = fcvt_to_integer<int64_t>(rs1.f32[0], rmm, invalid);
				break;
			case 0x3: // FCVT.LU.S
				dst = fcvt_to_integer<uint64_t>(rs1.f32[0], rmm, invalid);
				break;
			default:
				cpu.trigger_exception(ILLEGAL_OPERATION);
			}
			break;
		case 0x1: // from float64
			switch (fi.R4type.rs2) {
			case 0x0: // FCVT.W.D (sign-extended 32-bit result)
				dst = fcvt_to_integer<int32_t>(rs1.f64, rmm, invalid);
				break;
			case 0x1: // FCVT.WU.D (sign-extended 32-bit result)
				dst = int32_t(fcvt_to_integer<uint32_t>(rs1.f64, rmm, invalid));
				break;
			case 0x2: // FCVT.L.D
				dst = fcvt_to_integer<int64_t>(rs1.f64, rmm, invalid);
				break;
			case 0x3: // FCVT.LU.D
				dst = fcvt_to_integer<uint64_t>(rs1.f64, rmm, invalid);
				break;
			default:
				cpu.trigger_exception(ILLEGAL_OPERATION);
			}
			break;
		default:
			cpu.trigger_exception(ILLEGAL_OPERATION);
			return;
		}
		if constexpr (fcsr_emulation) {
			if (invalid)
				cpu.registers().fcsr().fflags |= 16; // NV
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		const rv32f_instruction fi { instr };
		static const std::array<const char*, 4> f2 {
			"FCVT.W.S", "FCVT.W.D", "???", "FCVT.W.Q"
		};
		return snprintf(buffer, len, "%s %s, %s", f2[fi.R4type.funct2],
						RISCV::flpname(fi.R4type.rs1),
						RISCV::regname(fi.R4type.rd));
	});

	FLOAT_INSTR(FCVT_SD_W,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		auto& rs1 = cpu.reg(fi.R4type.rs1);
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		bool inexact = false;
		switch (fi.R4type.funct2) {
		case 0x0: // to float32
			switch (fi.R4type.rs2) {
			case 0x0: // FCVT.S.W
				dst.set_float(fcvt_from_integer<float>((int32_t)rs1, inexact));
				break;
			case 0x1: // FCVT.S.WU
				dst.set_float(fcvt_from_integer<float>((uint32_t)rs1, inexact));
				break;
			case 0x2: // FCVT.S.L
				dst.set_float(fcvt_from_integer<float>((int64_t)rs1, inexact));
				break;
			case 0x3: // FCVT.S.LU
				dst.set_float(fcvt_from_integer<float>((uint64_t)rs1, inexact));
				break;
			default:
				cpu.trigger_exception(ILLEGAL_OPERATION);
			}
			break;
		case 0x1: // to float64
			switch (fi.R4type.rs2) {
			case 0x0: // FCVT.D.W
				dst.f64 = fcvt_from_integer<double>((int32_t)rs1, inexact);
				break;
			case 0x1: // FCVT.D.WU
				dst.f64 = fcvt_from_integer<double>((uint32_t)rs1, inexact);
				break;
			case 0x2: // FCVT.D.L
				dst.f64 = fcvt_from_integer<double>((int64_t)rs1, inexact);
				break;
			case 0x3: // FCVT.D.LU
				dst.f64 = fcvt_from_integer<double>((uint64_t)rs1, inexact);
				break;
			default:
				cpu.trigger_exception(ILLEGAL_OPERATION);
			}
			break;
		default:
			cpu.trigger_exception(ILLEGAL_OPERATION);
			return;
		}
		if constexpr (fcsr_emulation) {
			if (inexact)
				cpu.registers().fcsr().fflags |= 1; // NX
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		const rv32f_instruction fi { instr };
		static const std::array<const char*, 4> f2 {
			"FCVT.S.W", "FCVT.D.W", "???", "FCVT.Q.W"
		};
		return snprintf(buffer, len, "%s %s, %s", f2[fi.R4type.funct2],
						RISCV::regname(fi.R4type.rs1),
						RISCV::flpname(fi.R4type.rd));
	});

	FLOAT_INSTR(FSGNJ_NX,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
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
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
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

	FLOAT_INSTR(FCLASS, // 1110 f3 = 0x1
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		auto& dst = cpu.reg(fi.R4type.rd);
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		// FCLASS sets exactly one bit, derived from the raw sign/exponent/fraction
		// fields. Host floating-point comparisons cannot be used here, as they
		// don't distinguish subnormals from normals, or sNaN from qNaN.
		switch (fi.R4type.funct2) {
		case 0x0: // FCLASS.S
			dst = rv_fclass32(rs1.i32[0]);
			return;
		case 0x1: // FCLASS.D
			dst = rv_fclass64(rs1.i64);
			return;
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		const rv32f_instruction fi { instr };
		static const std::array<const char*, 4> f2 {
			"FCLASS.S", "FCLASS.D", "???", "FCLASS.Q"
		};
		return snprintf(buffer, len, "%s %s, %s", f2[fi.R4type.funct2],
						RISCV::flpname(fi.R4type.rs1),
						RISCV::regname(fi.R4type.rd));
	});

	FLOAT_INSTR(FMV_X_W, // 1110 f3 = 0x0
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		auto& dst = cpu.reg(fi.R4type.rd);
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		switch (fi.R4type.funct2) {
		case 0x0: // FMV.X.W
// FMV.X.W moves the single-precision value in floating-point register rs1 represented in IEEE 754-
// 2008 encoding to the lower 32 bits of integer register rd. The bits are not modified in the transfer,
// and in particular, the payloads of non-canonical NaNs are preserved. For RV64, the higher 32 bits
// of the destination register are filled with copies of the floating-point number’s sign bit.
			dst = RVSIGNTYPE(cpu)(rs1.i32[0]);
			return;
		case 0x1: // FMV.X.D
			if constexpr (RVISGE64BIT(cpu)) {
				dst = RVSIGNTYPE(cpu)(rs1.i64);
				return;
			}
			break;
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		const rv32f_instruction fi { instr };
		static const std::array<const char*, 4> f2 {
			"FMV.X.W", "FMV.X.D", "???", "FMV.X.Q"
		};
		return snprintf(buffer, len, "%s %s, %s", f2[fi.R4type.funct2],
						RISCV::flpname(fi.R4type.rs1),
						RISCV::regname(fi.R4type.rd));
	});

	FLOAT_INSTR(FMV_W_X, // 1111
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		auto& rs1 = cpu.reg(fi.R4type.rs1);
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		switch (fi.R4type.funct2) {
		case 0x0: // FMV.W.X
			dst.load_u32(rs1);
			return;
		case 0x1: // FMV.D.X
			if constexpr (RVISGE64BIT(cpu)) {
				dst.load_u64(rs1);
				return;
			}
			break;
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		const rv32f_instruction fi { instr };
		static const std::array<const char*, 4> f2 {
			"FMV.W.X", "FMV.D.X", "???", "FMV.Q.X"
		};
		return snprintf(buffer, len, "%s %s, %s", f2[fi.R4type.funct2],
						RISCV::regname(fi.R4type.rs1),
						RISCV::flpname(fi.R4type.rd));
	});
}
