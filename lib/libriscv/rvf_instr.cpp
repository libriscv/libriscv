#include "rvfd.hpp"
#include "fp16.hpp"
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
	static inline F fcvt_from_integer(T value, unsigned rm, bool& inexact) {
		const F converted = F(value);
		if constexpr (fcsr_emulation) {
			const long double exact = (long double)value;
			inexact = (long double)converted != exact;
			if (inexact) {
				// The host cast is round-to-nearest-even; adjust the result
				// for the other RISC-V rounding modes using the float grid.
				const long double cld = (long double)converted;
				switch (rm) {
				case 0x1: // RTZ: toward zero
					// Adjust only when the RNE result moved away from zero
					// (|cld| > |exact|); when it already rounded toward zero
					// (|cld| < |exact|) the RNE result is the RTZ result.
					if (exact > 0.0L && cld > exact) return std::nextafter(converted, F(0.0f));
					if (exact < 0.0L && cld < exact) return std::nextafter(converted, F(0.0f));
					break;
				case 0x2: // RDN: toward -inf
					if (cld > exact) return std::nextafter(converted, F(-INFINITY));
					break;
				case 0x3: // RUP: toward +inf
					if (cld < exact) return std::nextafter(converted, F(INFINITY));
					break;
				case 0x4: { // RMM: ties away from zero
					// RMM differs from RNE *only* at an exact halfway point,
					// where it takes the larger magnitude instead of the even
					// significand. Everywhere else nearest is nearest, so the
					// host cast is already right.
					if (fabsl(cld) < fabsl(exact)) {
						const F away = std::nextafter(converted,
							converted < F(0.0) ? F(-INFINITY) : F(INFINITY));
						if (fabsl((long double)away - exact) == fabsl(exact - cld))
							return away;
					}
					break;
				}
				default: break; // RNE (0)
				}
			}
		}
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
		return rv_expect_mnemonic(buffer, len, "flw",
			RVFDISASM::op_load(buffer, len, instr));
	});
	FLOAT_INSTR(FLD,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		auto addr = cpu.reg(fi.Itype.rs1) + fi.Itype.signed_imm();
		auto& dst = cpu.registers().getfl(fi.Itype.rd);
		dst.load_u64(cpu.machine().memory.template read<uint64_t> (addr));
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		return rv_expect_mnemonic(buffer, len, "fld",
			RVFDISASM::op_load(buffer, len, instr));
	});

	/* Zfhmin: the half-precision load and store. The extension has no
	 * arithmetic of its own -- these two, the register moves and the
	 * conversions are all of it -- so a half only ever travels between
	 * memory and a NaN-boxed register, or through a conversion. */
	FLOAT_INSTR(FLH,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		auto addr = cpu.reg(fi.Itype.rs1) + fi.Itype.signed_imm();
		auto& dst = cpu.registers().getfl(fi.Itype.rd);
		dst.load_u16(cpu.machine().memory.template read<uint16_t> (addr));
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		return rv_expect_mnemonic(buffer, len, "flh",
			RVFDISASM::op_load(buffer, len, instr));
	});

	FLOAT_INSTR(FSH,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		const auto& src = cpu.registers().getfl(fi.Stype.rs2);
		auto addr = cpu.reg(fi.Stype.rs1) + fi.Stype.signed_imm();
		// A store takes the low sixteen bits as they are, boxed or not.
		cpu.machine().memory.template write<uint16_t> (addr, uint16_t(src.i32[0]));
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		return rv_expect_mnemonic(buffer, len, "fsh",
			RVFDISASM::op_store(buffer, len, instr));
	});

	FLOAT_INSTR(FSW,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		const auto& src = cpu.registers().getfl(fi.Stype.rs2);
		auto addr = cpu.reg(fi.Stype.rs1) + fi.Stype.signed_imm();
		cpu.machine().memory.template write<uint32_t> (addr, src.i32[0]);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		return rv_expect_mnemonic(buffer, len, "fsw",
			RVFDISASM::op_store(buffer, len, instr));
	});
	FLOAT_INSTR(FSD,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		const auto& src = cpu.registers().getfl(fi.Stype.rs2);
		auto addr = cpu.reg(fi.Stype.rs1) + fi.Stype.signed_imm();
		cpu.machine().memory.template write<uint64_t> (addr, src.i64);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		return rv_expect_mnemonic(buffer, len, "fsd",
			RVFDISASM::op_store(buffer, len, instr));
	});

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
			// RV64 NaN-boxing: an operand whose upper 32 bits are not all
			// ones is read as the canonical quiet NaN, not as its low word.
			if constexpr (RVISGE64BIT(cpu) && nanboxing) {
				if (UNLIKELY(static_cast<uint32_t>(rs1.i32[1]) != 0xFFFFFFFFu
					|| static_cast<uint32_t>(rs2.i32[1]) != 0xFFFFFFFFu
					|| static_cast<uint32_t>(rs3.i32[1]) != 0xFFFFFFFFu)) {
					dst.load_u32(CANONICAL_NAN_F32);
					return;
				}
			}
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
		return rv_expect_mnemonic(buffer, len, "fmadd",
			RVFDISASM::op_fused(buffer, len, instr));
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
			// RV64 NaN-boxing: an operand whose upper 32 bits are not all
			// ones is read as the canonical quiet NaN, not as its low word.
			if constexpr (RVISGE64BIT(cpu) && nanboxing) {
				if (UNLIKELY(static_cast<uint32_t>(rs1.i32[1]) != 0xFFFFFFFFu
					|| static_cast<uint32_t>(rs2.i32[1]) != 0xFFFFFFFFu
					|| static_cast<uint32_t>(rs3.i32[1]) != 0xFFFFFFFFu)) {
					dst.load_u32(CANONICAL_NAN_F32);
					return;
				}
			}
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
		return rv_expect_mnemonic(buffer, len, "fmsub",
			RVFDISASM::op_fused(buffer, len, instr));
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
			// RV64 NaN-boxing: an operand whose upper 32 bits are not all
			// ones is read as the canonical quiet NaN, not as its low word.
			if constexpr (RVISGE64BIT(cpu) && nanboxing) {
				if (UNLIKELY(static_cast<uint32_t>(rs1.i32[1]) != 0xFFFFFFFFu
					|| static_cast<uint32_t>(rs2.i32[1]) != 0xFFFFFFFFu
					|| static_cast<uint32_t>(rs3.i32[1]) != 0xFFFFFFFFu)) {
					dst.load_u32(CANONICAL_NAN_F32);
					return;
				}
			}
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
		return rv_expect_mnemonic(buffer, len, "fnmadd",
			RVFDISASM::op_fused(buffer, len, instr));
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
			// RV64 NaN-boxing: an operand whose upper 32 bits are not all
			// ones is read as the canonical quiet NaN, not as its low word.
			if constexpr (RVISGE64BIT(cpu) && nanboxing) {
				if (UNLIKELY(static_cast<uint32_t>(rs1.i32[1]) != 0xFFFFFFFFu
					|| static_cast<uint32_t>(rs2.i32[1]) != 0xFFFFFFFFu
					|| static_cast<uint32_t>(rs3.i32[1]) != 0xFFFFFFFFu)) {
					dst.load_u32(CANONICAL_NAN_F32);
					return;
				}
			}
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
		return rv_expect_mnemonic(buffer, len, "fnmsub",
			RVFDISASM::op_fused(buffer, len, instr));
	});

	FLOAT_INSTR(FADD,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		if (fi.R4type.funct2 == 0x0) { // float32
			// RV64 NaN-boxing: an operand whose upper 32 bits are not all
			// ones is read as the canonical quiet NaN, not as its low word.
			if constexpr (RVISGE64BIT(cpu) && nanboxing) {
				if (UNLIKELY(static_cast<uint32_t>(rs1.i32[1]) != 0xFFFFFFFFu
					|| static_cast<uint32_t>(rs2.i32[1]) != 0xFFFFFFFFu)) {
					dst.load_u32(CANONICAL_NAN_F32);
					return;
				}
			}
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
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return rv_expect_mnemonic(buffer, len, "fadd",
			RVFDISASM::op_fp(buffer, len, instr, RVISGE64BIT(cpu)));
	});

	FLOAT_INSTR(FSUB,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		if (fi.R4type.funct2 == 0x0) { // float32
			// RV64 NaN-boxing: an operand whose upper 32 bits are not all
			// ones is read as the canonical quiet NaN, not as its low word.
			if constexpr (RVISGE64BIT(cpu) && nanboxing) {
				if (UNLIKELY(static_cast<uint32_t>(rs1.i32[1]) != 0xFFFFFFFFu
					|| static_cast<uint32_t>(rs2.i32[1]) != 0xFFFFFFFFu)) {
					dst.load_u32(CANONICAL_NAN_F32);
					return;
				}
			}
			dst.set_float(rs1.f32[0] - rs2.f32[0]);
			fsflags(cpu, (double)(rs1.f32[0]) - (double)(rs2.f32[0]), dst.f32[0]);
		} else if (fi.R4type.funct2 == 0x1) { // float64
			dst.f64 = rs1.f64 - rs2.f64;
			fsflags(cpu, (long double)(rs1.f64) - (long double)(rs2.f64), dst.f64);
		} else {
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return rv_expect_mnemonic(buffer, len, "fsub",
			RVFDISASM::op_fp(buffer, len, instr, RVISGE64BIT(cpu)));
	});

	FLOAT_INSTR(FMUL,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		if (fi.R4type.funct2 == 0x0) { // float32
			// RV64 NaN-boxing: an operand whose upper 32 bits are not all
			// ones is read as the canonical quiet NaN, not as its low word.
			if constexpr (RVISGE64BIT(cpu) && nanboxing) {
				if (UNLIKELY(static_cast<uint32_t>(rs1.i32[1]) != 0xFFFFFFFFu
					|| static_cast<uint32_t>(rs2.i32[1]) != 0xFFFFFFFFu)) {
					dst.load_u32(CANONICAL_NAN_F32);
					return;
				}
			}
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
					if (result == 0x7f800000u) {
						cpu.registers().fcsr().fflags |= 4; // OF
						// The overflow value depends on the rounding mode
						// (IEEE 754 §7.4): RTZ saturates to the largest finite
						// number for either sign, RDN only for a positive
						// result and RUP only for a negative one. RNE and RMM
						// keep the infinity the host FPU produced.
						const unsigned rm = cpu.registers().fcsr().frm;
						const bool neg = (dst.i32[0] & 0x80000000u) != 0;
						if (rm == 0x1 || (rm == 0x2 && !neg) || (rm == 0x3 && neg)) {
							dst.i32[0] = (dst.i32[0] & 0x80000000u) | 0x7F7FFFFFu;
						}
					}
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
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return rv_expect_mnemonic(buffer, len, "fmul",
			RVFDISASM::op_fp(buffer, len, instr, RVISGE64BIT(cpu)));
	});

	FLOAT_INSTR(FDIV,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		if (fi.R4type.funct2 == 0x0) { // fp32
			// RV64 NaN-boxing: an operand whose upper 32 bits are not all
			// ones is read as the canonical quiet NaN, not as its low word.
			if constexpr (RVISGE64BIT(cpu) && nanboxing) {
				if (UNLIKELY(static_cast<uint32_t>(rs1.i32[1]) != 0xFFFFFFFFu
					|| static_cast<uint32_t>(rs2.i32[1]) != 0xFFFFFFFFu)) {
					dst.load_u32(CANONICAL_NAN_F32);
					return;
				}
			}
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
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return rv_expect_mnemonic(buffer, len, "fdiv",
			RVFDISASM::op_fp(buffer, len, instr, RVISGE64BIT(cpu)));
	});

	FLOAT_INSTR(FSQRT,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		switch (fi.R4type.funct2) {
		case 0x0: // FSQRT.S
			// RV64 NaN-boxing: an operand whose upper 32 bits are not all
			// ones is read as the canonical quiet NaN, not as its low word.
			if constexpr (RVISGE64BIT(cpu) && nanboxing) {
				if (UNLIKELY(static_cast<uint32_t>(rs1.i32[1]) != 0xFFFFFFFFu)) {
					dst.load_u32(CANONICAL_NAN_F32);
					return;
				}
			}
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
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return rv_expect_mnemonic(buffer, len, "fsqrt",
			RVFDISASM::op_fp(buffer, len, instr, RVISGE64BIT(cpu)));
	});

	FLOAT_INSTR(FMIN_FMAX,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_COLDATTR
	{
		const rv32f_instruction fi { instr };
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		auto& dst = cpu.registers().getfl(fi.R4type.rd);

		// RV64 NaN-boxing: an operand whose upper 32 bits are not all ones
		// is read as the canonical quiet NaN, not as its low word. That also
		// makes the FMIN/FMAX NaN-propagation agree with the spec: a single
		// non-boxed operand yields the other operand, and a quiet NaN never
		// raises NV.
		if constexpr (RVISGE64BIT(cpu) && nanboxing) {
			if (fi.R4type.funct2 == 0x0
				&& UNLIKELY(static_cast<uint32_t>(rs1.i32[1]) != 0xFFFFFFFFu
					|| static_cast<uint32_t>(rs2.i32[1]) != 0xFFFFFFFFu)) {
				const bool nb1 = static_cast<uint32_t>(rs1.i32[1]) != 0xFFFFFFFFu;
				const bool nb2 = static_cast<uint32_t>(rs2.i32[1]) != 0xFFFFFFFFu;
				if (nb1 && nb2)
					dst.load_u32(CANONICAL_NAN_F32);
				else if (nb1)
					dst.load_u32(rs2.i32[0]);
				else
					dst.load_u32(rs1.i32[0]);
				if constexpr (fcsr_emulation)
					cpu.registers().fcsr().fflags = 0;
				return;
			}
		}

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
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVFDISASM::op_fp(buffer, len, instr, RVISGE64BIT(cpu));
	});

	FLOAT_INSTR(FEQ_FLT_FLE,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		auto& dst = cpu.reg(fi.R4type.rd);

		// RV64 NaN-boxing: an operand whose upper 32 bits are not all ones
		// is read as the canonical quiet NaN, not as its low word. All
		// compares then return false; a quiet NaN input does not raise NV,
		// so clear the flags the same way feqflags would.
		if constexpr (RVISGE64BIT(cpu) && nanboxing) {
			if (fi.R4type.funct2 == 0x0
				&& UNLIKELY(static_cast<uint32_t>(rs1.i32[1]) != 0xFFFFFFFFu
					|| static_cast<uint32_t>(rs2.i32[1]) != 0xFFFFFFFFu)) {
				dst = 0;
				if constexpr (fcsr_emulation) {
					// FLE/FLT are signaling compares: any NaN operand
					// (including the canonical qNaN a non-boxed operand is
					// read as) raises NV. FEQ is quiet: only sNaN raises NV.
					const unsigned op = fi.R4type.funct3 | (fi.R4type.funct2 << 4);
					if (op == 0x0 || op == 0x1 || op == 0x10 || op == 0x11)
						cpu.registers().fcsr().fflags = 16; // NV
					else
						cpu.registers().fcsr().fflags = 0;
				}
				return;
			}
		}

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
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVFDISASM::op_fp(buffer, len, instr, RVISGE64BIT(cpu));
	});

	FLOAT_INSTR(FCVT_SD_DS,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		// Zfhmin adds three of the four half-precision conversions to this
		// group; rs2 names the source format the same way funct2 names the
		// destination, so a half on either side is rs2 == 2 or funct2 == 2.
		if (fi.R4type.rs2 == 0x2 || fi.R4type.funct2 == 0x2) {
			switch ((fi.R4type.funct2 << 3) | fi.R4type.rs2) {
			case (0x0 << 3) | 0x2: // FCVT.S.H
				dst.set_float(fp16::to_f32(rs1.get_half()));
				return;
			case (0x1 << 3) | 0x2: // FCVT.D.H
				dst.set_double(fp16::to_f64(rs1.get_half()));
				return;
			case (0x2 << 3) | 0x0: // FCVT.H.S
				// The single-precision source is itself NaN-boxed on RV64,
				// and an unboxed one is the canonical NaN.
				if constexpr (RVISGE64BIT(cpu) && nanboxing) {
					if (UNLIKELY(static_cast<uint32_t>(rs1.i32[1]) != 0xFFFFFFFFu)) {
						dst.load_u16(0x7E00);
						return;
					}
				}
				dst.load_u16(fp16::from_f32(rs1.f32[0]));
				return;
			case (0x2 << 3) | 0x1: // FCVT.H.D
				// Narrowing in one step: going via f32 would round twice.
				dst.load_u16(fp16::from_f64(rs1.f64));
				return;
			}
			cpu.trigger_exception(ILLEGAL_OPERATION);
			return;
		}
		// rs2 names the source format: the only pair left is single and
		// double, so it must be whichever of the two the destination is not.
		if (fi.R4type.rs2 != (fi.R4type.funct2 ^ 1)) {
			cpu.trigger_exception(ILLEGAL_OPERATION);
			return;
		}
		switch (fi.R4type.funct2) {
		case 0x0: // FCVT.S.D (64 -> 32)
			if (std::isnan(rs1.f64))
				dst.load_u32(CANONICAL_NAN_F32);
			else
				dst.set_float(rs1.f64);
			break;
		case 0x1: // FCVT.D.S (32 -> 64)
			// RV64 NaN-boxing: an operand whose upper 32 bits are not all
			// ones is read as the canonical quiet NaN, not as its low word.
			if constexpr (RVISGE64BIT(cpu) && nanboxing) {
				if (UNLIKELY(static_cast<uint32_t>(rs1.i32[1]) != 0xFFFFFFFFu)) {
					dst.load_u64(CANONICAL_NAN_F64);
					break;
				}
			}
			if (std::isnan(rs1.f32[0]))
				dst.load_u64(CANONICAL_NAN_F64);
			else
				dst.f64 = rs1.f32[0];
			break;
		default:
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return rv_expect_mnemonic(buffer, len, "fcvt",
			RVFDISASM::op_fp(buffer, len, instr, RVISGE64BIT(cpu)));
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
		bool invalid = false, inexact = false;
		switch (fi.R4type.funct2) {
		case 0x0: { // from float32
			// RV64 NaN-boxing: an operand whose upper 32 bits are not all
			// ones is read as the canonical quiet NaN, not as its low word.
			// A NaN operand converts to the maximum value and raises NV,
			// which fcvt_to_integer() below already does for qNaN. The source
			// register itself must not be modified, so substitute a local.
			float src = rs1.f32[0];
			if constexpr (RVISGE64BIT(cpu) && nanboxing) {
				if (UNLIKELY(static_cast<uint32_t>(rs1.i32[1]) != 0xFFFFFFFFu))
					__builtin_memcpy(&src, &CANONICAL_NAN_F32, sizeof(src));
			}
			switch (fi.R4type.rs2) {
			case 0x0: // FCVT.W.S (sign-extended 32-bit result)
				dst = fcvt_to_integer<int32_t>(src, rmm, invalid, inexact);
				break;
			case 0x1: // FCVT.WU.S (sign-extended 32-bit result)
				dst = int32_t(fcvt_to_integer<uint32_t>(src, rmm, invalid, inexact));
				break;
			case 0x2: // FCVT.L.S
				dst = fcvt_to_integer<int64_t>(src, rmm, invalid, inexact);
				break;
			case 0x3: // FCVT.LU.S
				dst = fcvt_to_integer<uint64_t>(src, rmm, invalid, inexact);
				break;
			default:
				cpu.trigger_exception(ILLEGAL_OPERATION);
			}
			break;
		}
		case 0x1: // from float64
			switch (fi.R4type.rs2) {
			case 0x0: // FCVT.W.D (sign-extended 32-bit result)
				dst = fcvt_to_integer<int32_t>(rs1.f64, rmm, invalid, inexact);
				break;
			case 0x1: // FCVT.WU.D (sign-extended 32-bit result)
				dst = int32_t(fcvt_to_integer<uint32_t>(rs1.f64, rmm, invalid, inexact));
				break;
			case 0x2: // FCVT.L.D
				dst = fcvt_to_integer<int64_t>(rs1.f64, rmm, invalid, inexact);
				break;
			case 0x3: // FCVT.LU.D
				dst = fcvt_to_integer<uint64_t>(rs1.f64, rmm, invalid, inexact);
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
			else if (inexact)
				cpu.registers().fcsr().fflags |= 1; // NX
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return rv_expect_mnemonic(buffer, len, "fcvt",
			RVFDISASM::op_fp(buffer, len, instr, RVISGE64BIT(cpu)));
	});

	FLOAT_INSTR(FCVT_SD_W,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		auto& rs1 = cpu.reg(fi.R4type.rs1);
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		bool inexact = false;
		auto rmm = fi.R4type.funct3; // rounding mode is encoded in funct3
		if (rmm == 0x7) // DYN: use the dynamic mode from the fcsr CSR (frm field)
			rmm = cpu.registers().fcsr().frm;
		switch (fi.R4type.funct2) {
		case 0x0: // to float32
			switch (fi.R4type.rs2) {
			case 0x0: // FCVT.S.W
				dst.set_float(fcvt_from_integer<float>((int32_t)rs1, rmm, inexact));
				break;
			case 0x1: // FCVT.S.WU
				dst.set_float(fcvt_from_integer<float>((uint32_t)rs1, rmm, inexact));
				break;
			case 0x2: // FCVT.S.L
				dst.set_float(fcvt_from_integer<float>((int64_t)rs1, rmm, inexact));
				break;
			case 0x3: // FCVT.S.LU
				dst.set_float(fcvt_from_integer<float>((uint64_t)rs1, rmm, inexact));
				break;
			default:
				cpu.trigger_exception(ILLEGAL_OPERATION);
			}
			break;
		case 0x1: // to float64
			switch (fi.R4type.rs2) {
			case 0x0: // FCVT.D.W
				dst.f64 = fcvt_from_integer<double>((int32_t)rs1, rmm, inexact);
				break;
			case 0x1: // FCVT.D.WU
				dst.f64 = fcvt_from_integer<double>((uint32_t)rs1, rmm, inexact);
				break;
			case 0x2: // FCVT.D.L
				dst.f64 = fcvt_from_integer<double>((int64_t)rs1, rmm, inexact);
				break;
			case 0x3: // FCVT.D.LU
				dst.f64 = fcvt_from_integer<double>((uint64_t)rs1, rmm, inexact);
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
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return rv_expect_mnemonic(buffer, len, "fcvt",
			RVFDISASM::op_fp(buffer, len, instr, RVISGE64BIT(cpu)));
	});

	FLOAT_INSTR(FSGNJ_NX,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32f_instruction fi { instr };
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		// RV64 NaN-boxing: an operand whose upper 32 bits are not all ones
		// is read as the canonical quiet NaN, not as its low word. Both
		// source registers are checked: the sign-selecting operand rs2 is
		// read as a NaN too, so any non-boxed input makes the result NaN.
		if constexpr (RVISGE64BIT(cpu) && nanboxing) {
			if (fi.R4type.funct2 == 0x0
				&& UNLIKELY(static_cast<uint32_t>(rs1.i32[1]) != 0xFFFFFFFFu
					|| static_cast<uint32_t>(rs2.i32[1]) != 0xFFFFFFFFu)) {
				// FSGNJN inverts the sign bit of the NaN result (§11.6).
				dst.load_u32(fi.R4type.funct3 == 0x1 ? 0xFFC00000u : CANONICAL_NAN_F32);
				return;
			}
		}
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
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVFDISASM::op_fp(buffer, len, instr, RVISGE64BIT(cpu));
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
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return rv_expect_mnemonic(buffer, len, "fclass",
			RVFDISASM::op_fp(buffer, len, instr, RVISGE64BIT(cpu)));
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
		case 0x2: // FMV.X.H (Zfhmin)
			// Like FMV.X.W, the bits move untouched and the sign extends
			// across the rest of the destination. The box is not checked:
			// this is a bit move, not a read of a half-precision value.
			dst = RVSIGNTYPE(cpu)(int16_t(rs1.i32[0]));
			return;
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return rv_expect_mnemonic(buffer, len, "fmv",
			RVFDISASM::op_fp(buffer, len, instr, RVISGE64BIT(cpu)));
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
		case 0x2: // FMV.H.X (Zfhmin)
			dst.load_u16(uint16_t(rs1));
			return;
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return rv_expect_mnemonic(buffer, len, "fmv",
			RVFDISASM::op_fp(buffer, len, instr, RVISGE64BIT(cpu)));
	});

	static constexpr uint32_t FLI_TABLE_S[32] = {
		0xBF800000u, 0x00800000u, 0x37800000u, 0x38000000u,
		0x3B800000u, 0x3C000000u, 0x3D800000u, 0x3E000000u,
		0x3E800000u, 0x3EA00000u, 0x3EC00000u, 0x3EE00000u,
		0x3F000000u, 0x3F200000u, 0x3F400000u, 0x3F600000u,
		0x3F800000u, 0x3FA00000u, 0x3FC00000u, 0x3FE00000u,
		0x40000000u, 0x40200000u, 0x40400000u, 0x40800000u,
		0x41000000u, 0x41800000u, 0x43000000u, 0x43800000u,
		0x47000000u, 0x47800000u, 0x7F800000u, 0x7FC00000u
	};
	static constexpr uint64_t FLI_TABLE_D[32] = {
		0xBFF0000000000000ull, 0x0010000000000000ull,
		0x3EF0000000000000ull, 0x3F00000000000000ull,
		0x3F70000000000000ull, 0x3F80000000000000ull,
		0x3FB0000000000000ull, 0x3FC0000000000000ull,
		0x3FD0000000000000ull, 0x3FD4000000000000ull,
		0x3FD8000000000000ull, 0x3FDC000000000000ull,
		0x3FE0000000000000ull, 0x3FE4000000000000ull,
		0x3FE8000000000000ull, 0x3FEC000000000000ull,
		0x3FF0000000000000ull, 0x3FF4000000000000ull,
		0x3FF8000000000000ull, 0x3FFC000000000000ull,
		0x4000000000000000ull, 0x4004000000000000ull,
		0x4008000000000000ull, 0x4010000000000000ull,
		0x4020000000000000ull, 0x4030000000000000ull,
		0x4060000000000000ull, 0x4070000000000000ull,
		0x40E0000000000000ull, 0x40F0000000000000ull,
		0x7FF0000000000000ull, 0x7FF8000000000000ull
	};

	FLOAT_INSTR(FLI,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_COLDATTR
	{
		const rv32f_instruction fi { instr };
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		switch (fi.R4type.funct2) {
		case 0x0:
			dst.load_u32(FLI_TABLE_S[fi.R4type.rs1]);
			return;
		case 0x1:
			dst.load_u64(FLI_TABLE_D[fi.R4type.rs1]);
			return;
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return rv_expect_mnemonic(buffer, len, "fli",
			RVFDISASM::op_fp(buffer, len, instr, RVISGE64BIT(cpu)));
	});

	FLOAT_INSTR(FMINM_FMAXM,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_COLDATTR
	{
		const rv32f_instruction fi { instr };
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		const bool is_max = (fi.R4type.funct3 & 1) != 0;

		switch (fi.R4type.funct2) {
		case 0x0: {
			uint32_t ab = rs1.i32[0];
			uint32_t bb = rs2.i32[0];
			if constexpr (RVISGE64BIT(cpu) && nanboxing) {
				if (UNLIKELY(static_cast<uint32_t>(rs1.i32[1]) != 0xFFFFFFFFu))
					ab = CANONICAL_NAN_F32;
				if (UNLIKELY(static_cast<uint32_t>(rs2.i32[1]) != 0xFFFFFFFFu))
					bb = CANONICAL_NAN_F32;
			}
			float a, b;
			__builtin_memcpy(&a, &ab, 4);
			__builtin_memcpy(&b, &bb, 4);
			const bool snan = !fcsr_emulation ? false
				: (is_signaling_nan(a) || is_signaling_nan(b));
			uint32_t rb;
			if (std::isnan(a) || std::isnan(b)) {
				rb = CANONICAL_NAN_F32;
			} else if (a == 0.0f && b == 0.0f) {
				rb = is_max
					? (((ab & bb) & 0x80000000u) ? 0x80000000u : 0x00000000u)
					: (((ab | bb) & 0x80000000u) ? 0x80000000u : 0x00000000u);
			} else {
				const float r = is_max ? std::fmax(a, b) : std::fmin(a, b);
				__builtin_memcpy(&rb, &r, 4);
			}
			dst.load_u32(rb);
			if constexpr (fcsr_emulation)
				cpu.registers().fcsr().fflags = snan ? 16 : 0;
			return;
		}
		case 0x1: {
			const uint64_t ab = rs1.i64;
			const uint64_t bb = rs2.i64;
			const double a = rs1.f64;
			const double b = rs2.f64;
			const bool snan = !fcsr_emulation ? false
				: (is_signaling_nan(a) || is_signaling_nan(b));
			uint64_t rb;
			if (std::isnan(a) || std::isnan(b)) {
				rb = CANONICAL_NAN_F64;
			} else if (a == 0.0 && b == 0.0) {
				rb = is_max
					? (((ab & bb) & 0x8000000000000000ull) ? 0x8000000000000000ull : 0x0ull)
					: (((ab | bb) & 0x8000000000000000ull) ? 0x8000000000000000ull : 0x0ull);
			} else {
				const double r = is_max ? std::fmax(a, b) : std::fmin(a, b);
				__builtin_memcpy(&rb, &r, 8);
			}
			dst.load_u64(rb);
			if constexpr (fcsr_emulation)
				cpu.registers().fcsr().fflags = snan ? 16 : 0;
			return;
		}
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVFDISASM::op_fp(buffer, len, instr, RVISGE64BIT(cpu));
	});

	FLOAT_INSTR(FROUND,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_COLDATTR
	{
		const rv32f_instruction fi { instr };
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& dst = cpu.registers().getfl(fi.R4type.rd);
		auto rmm = fi.R4type.funct3;
		if (rmm == 0x7)
			rmm = cpu.registers().fcsr().frm;
		const bool signal_inexact = (fi.R4type.rs2 == 0x5);

		switch (fi.R4type.funct2) {
		case 0x0: {
			float src = rs1.f32[0];
			if constexpr (RVISGE64BIT(cpu) && nanboxing) {
				if (UNLIKELY(static_cast<uint32_t>(rs1.i32[1]) != 0xFFFFFFFFu))
					__builtin_memcpy(&src, &CANONICAL_NAN_F32, sizeof(src));
			}
			if (UNLIKELY(std::isnan(src))) {
				if constexpr (fcsr_emulation)
					cpu.registers().fcsr().fflags = is_signaling_nan(src) ? 16 : 0;
				dst.load_u32(CANONICAL_NAN_F32);
				return;
			}
			const float result = fcvt_round(src, rmm);
			if constexpr (fcsr_emulation)
				cpu.registers().fcsr().fflags =
					(signal_inexact && result != src) ? 1 : 0;
			dst.set_float(result);
			return;
		}
		case 0x1: {
			const double src = rs1.f64;
			if (UNLIKELY(std::isnan(src))) {
				if constexpr (fcsr_emulation)
					cpu.registers().fcsr().fflags = is_signaling_nan(src) ? 16 : 0;
				dst.load_u64(CANONICAL_NAN_F64);
				return;
			}
			const double result = fcvt_round(src, rmm);
			if constexpr (fcsr_emulation)
				cpu.registers().fcsr().fflags =
					(signal_inexact && result != src) ? 1 : 0;
			dst.set_double(result);
			return;
		}
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVFDISASM::op_fp(buffer, len, instr, RVISGE64BIT(cpu));
	});

	FLOAT_INSTR(FCVTMOD_W_D,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_COLDATTR
	{
		const rv32f_instruction fi { instr };
		const double src = cpu.registers().getfl(fi.R4type.rs1).f64;
		auto& dst = cpu.reg(fi.R4type.rd);
		bool invalid = false, inexact = false;
		int32_t result = 0;

		if (UNLIKELY(std::isnan(src) || std::isinf(src))) {
			invalid = true;
		} else {
			const double trunced = std::trunc(src);
			if (!(trunced >= -2147483648.0 && trunced < 2147483648.0))
				invalid = true;
			else if constexpr (fcsr_emulation)
				inexact = (trunced != src);
			const double wrapped = std::fmod(trunced, 4294967296.0);
			result = int32_t(uint32_t(int64_t(wrapped)));
		}
		dst = RVSIGNTYPE(cpu)(result);
		if constexpr (fcsr_emulation) {
			auto& fcsr = cpu.registers().fcsr();
			fcsr.fflags = 0;
			if (invalid)
				fcsr.fflags |= 16;
			else if (inexact)
				fcsr.fflags |= 1;
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return rv_expect_mnemonic(buffer, len, "fcvtmod",
			RVFDISASM::op_fp(buffer, len, instr, RVISGE64BIT(cpu)));
	});

	FLOAT_INSTR(FLEQ_FLTQ,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_COLDATTR
	{
		const rv32f_instruction fi { instr };
		auto& rs1 = cpu.registers().getfl(fi.R4type.rs1);
		auto& rs2 = cpu.registers().getfl(fi.R4type.rs2);
		auto& dst = cpu.reg(fi.R4type.rd);

		if constexpr (RVISGE64BIT(cpu) && nanboxing) {
			if (fi.R4type.funct2 == 0x0
				&& UNLIKELY(static_cast<uint32_t>(rs1.i32[1]) != 0xFFFFFFFFu
					|| static_cast<uint32_t>(rs2.i32[1]) != 0xFFFFFFFFu)) {
				dst = 0;
				if constexpr (fcsr_emulation)
					cpu.registers().fcsr().fflags = 0;
				return;
			}
		}

		switch (fi.R4type.funct3 | (fi.R4type.funct2 << 4))
		{
		case 0x4: // FLEQ.S
			dst = (rs1.f32[0] <= rs2.f32[0]) ? 1 : 0;
			feqflags<false>(cpu, rs1.f32[0], rs2.f32[0], dst);
			break;
		case 0x5: // FLTQ.S
			dst = (rs1.f32[0] < rs2.f32[0]) ? 1 : 0;
			feqflags<false>(cpu, rs1.f32[0], rs2.f32[0], dst);
			break;
		case 0x14: // FLEQ.D
			dst = (rs1.f64 <= rs2.f64) ? 1 : 0;
			feqflags<false>(cpu, rs1.f64, rs2.f64, dst);
			break;
		case 0x15: // FLTQ.D
			dst = (rs1.f64 < rs2.f64) ? 1 : 0;
			feqflags<false>(cpu, rs1.f64, rs2.f64, dst);
			break;
		default:
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVFDISASM::op_fp(buffer, len, instr, RVISGE64BIT(cpu));
	});

	FLOAT_INSTR(FMVH_X_D,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_COLDATTR
	{
		const rv32f_instruction fi { instr };
		if constexpr (RVIS32BIT(cpu)) {
			if (fi.R4type.funct2 == 0x1) {
				cpu.reg(fi.R4type.rd) = cpu.registers().getfl(fi.R4type.rs1).i32[1];
				return;
			}
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return rv_expect_mnemonic(buffer, len, "fmvh",
			RVFDISASM::op_fp(buffer, len, instr, RVISGE64BIT(cpu)));
	});

	FLOAT_INSTR(FMVP_D_X,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_COLDATTR
	{
		const rv32f_instruction fi { instr };
		if constexpr (RVIS32BIT(cpu)) {
			if (fi.R4type.funct2 == 0x1 && fi.R4type.funct3 == 0x0) {
				const uint64_t lower = uint32_t(cpu.reg(fi.R4type.rs1));
				const uint64_t upper = uint32_t(cpu.reg(fi.R4type.rs2));
				cpu.registers().getfl(fi.R4type.rd).load_u64((upper << 32) | lower);
				return;
			}
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return rv_expect_mnemonic(buffer, len, "fmvp",
			RVFDISASM::op_fp(buffer, len, instr, RVISGE64BIT(cpu)));
	});
}
