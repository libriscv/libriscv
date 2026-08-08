#pragma once
#include "common.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

// Floating-point semantics that RISC-V defines differently from the host, or
// that C++ leaves undefined. Shared by the interpreter (rvf_instr.cpp) and the
// asmjit backend (asmjit/aj_api.cpp), which must agree exactly: a guest is
// allowed to run the same function under both.

namespace riscv
{
	// RISC-V canonical NaNs
	static constexpr uint32_t CANONICAL_NAN_F32 = 0x7fc00000;
	static constexpr uint64_t CANONICAL_NAN_F64 = 0x7ff8000000000000ull;

	/// @brief Round a float/double per the RISC-V rounding mode.
	/// @param rm The rounding mode, as encoded in the funct3 of an FCVT.
	template <typename F>
	static inline F fcvt_round(F value, unsigned rm) {
		switch (rm) {
		case 0x1: // RTZ: round toward zero
			return std::trunc(value);
		case 0x2: // RDN: round down (toward -inf)
			return std::floor(value);
		case 0x3: // RUP: round up (toward +inf)
			return std::ceil(value);
		case 0x4: // RMM: round to nearest, ties away from zero
			return std::round(value);
		case 0x0: // RNE: round to nearest, ties to even
		default:  // reserved/invalid modes: nearest-even as the default
			return std::nearbyint(value);
		}
	}

	// Round per the RISC-V rounding mode, then convert to the destination
	// integer type T. The range check is *not* optional: converting an
	// out-of-range float to an integer is undefined behavior in C++, so a
	// guest could otherwise trip UBSan or produce host-dependent garbage.
	// RISC-V pins an out-of-range result to the nearest representable extreme
	// (NaN and positive overflow → maximum, negative overflow → minimum) and
	// raises NV, which is where the FCSR-only part starts: `invalid` and
	// `inexact` are only ever read under fcsr_emulation, and the NX test costs
	// an extra compare, so it is compiled out entirely when FCSR is off.
	template <typename T, typename F>
	static inline T fcvt_to_integer(F value, unsigned rm, bool& invalid, bool& inexact) {
		const F rounded = fcvt_round(value, rm);
		if constexpr (fcsr_emulation)
			inexact = !std::isnan(value) && (rounded != value);
		// Both bounds are powers of two, and thus exactly representable.
		constexpr F upper = std::is_signed<T>::value
			? F(uint64_t(1) << (sizeof(T) * 8 - 1))
			: F(uint64_t(1) << (sizeof(T) * 8 - 1)) * F(2);
		constexpr F lower = std::is_signed<T>::value ? -upper : F(0);
		if (UNLIKELY(!(rounded >= lower && rounded < upper))) { // NaN-safe
			invalid = true;
			return (std::isnan(rounded) || rounded >= upper)
				? std::numeric_limits<T>::max() : std::numeric_limits<T>::min();
		}
		return T(rounded);
	}
	template <typename T, typename F>
	static inline T fcvt_to_integer(F value, unsigned rm) {
		bool invalid = false, inexact = false;
		return fcvt_to_integer<T>(value, rm, invalid, inexact);
	}

	// FMIN/FMAX per RISC-V spec §11.6: -0.0 < +0.0, where IEEE 754
	// fmin/fmax leave the ±0 ordering implementation-defined. Two NaN operands
	// must produce the canonical qNaN, which is done unconditionally: it is the
	// only result a program can meaningfully use. A single NaN operand must
	// yield the other one, which std::fmin/fmax do not reliably do — glibc
	// returns a quieted copy of a signaling NaN instead. That distinction is
	// observable only through the NV flag, so it is gated on fcsr_emulation.
	static inline float rv_fmin32(float a, float b) noexcept {
		if (std::isnan(a) && std::isnan(b)) {
			float r; __builtin_memcpy(&r, &CANONICAL_NAN_F32, 4); return r;
		}
		if constexpr (fcsr_emulation) {
			if (std::isnan(a)) return b;
			if (std::isnan(b)) return a;
		}
		if (a == 0.0f && b == 0.0f) {
			uint32_t ab, bb;
			__builtin_memcpy(&ab, &a, 4); __builtin_memcpy(&bb, &b, 4);
			// -0 < +0 → return -0 if either is negative.
			const uint32_t out = ((ab | bb) & 0x80000000u) ? 0x80000000u : 0x0u;
			float r; __builtin_memcpy(&r, &out, 4); return r;
		}
		return std::fmin(a, b);
	}
	static inline float rv_fmax32(float a, float b) noexcept {
		if (std::isnan(a) && std::isnan(b)) {
			float r; __builtin_memcpy(&r, &CANONICAL_NAN_F32, 4); return r;
		}
		if constexpr (fcsr_emulation) {
			if (std::isnan(a)) return b;
			if (std::isnan(b)) return a;
		}
		if (a == 0.0f && b == 0.0f) {
			uint32_t ab, bb;
			__builtin_memcpy(&ab, &a, 4); __builtin_memcpy(&bb, &b, 4);
			// -0 < +0 → return +0 if either is non-negative.
			const uint32_t out = (~(ab & bb) & 0x80000000u) ? 0x0u : 0x80000000u;
			float r; __builtin_memcpy(&r, &out, 4); return r;
		}
		return std::fmax(a, b);
	}
	static inline double rv_fmin64(double a, double b) noexcept {
		if (std::isnan(a) && std::isnan(b)) {
			double r; __builtin_memcpy(&r, &CANONICAL_NAN_F64, 8); return r;
		}
		if constexpr (fcsr_emulation) {
			if (std::isnan(a)) return b;
			if (std::isnan(b)) return a;
		}
		if (a == 0.0 && b == 0.0) {
			uint64_t ab, bb;
			__builtin_memcpy(&ab, &a, 8); __builtin_memcpy(&bb, &b, 8);
			const uint64_t out = ((ab | bb) & 0x8000000000000000ull)
				? 0x8000000000000000ull : 0x0ull;
			double r; __builtin_memcpy(&r, &out, 8); return r;
		}
		return std::fmin(a, b);
	}
	static inline double rv_fmax64(double a, double b) noexcept {
		if (std::isnan(a) && std::isnan(b)) {
			double r; __builtin_memcpy(&r, &CANONICAL_NAN_F64, 8); return r;
		}
		if constexpr (fcsr_emulation) {
			if (std::isnan(a)) return b;
			if (std::isnan(b)) return a;
		}
		if (a == 0.0 && b == 0.0) {
			uint64_t ab, bb;
			__builtin_memcpy(&ab, &a, 8); __builtin_memcpy(&bb, &b, 8);
			const uint64_t out = (~(ab & bb) & 0x8000000000000000ull)
				? 0x0ull : 0x8000000000000000ull;
			double r; __builtin_memcpy(&r, &out, 8); return r;
		}
		return std::fmax(a, b);
	}

	// FCLASS sets exactly one bit, derived from the raw sign/exponent/fraction
	// fields. Host floating-point comparisons cannot be used here, as they
	// don't distinguish subnormals from normals, or sNaN from qNaN.
	static inline uint32_t rv_fclassify(bool sign, bool exp_max, bool exp_zero,
		bool frac_zero, bool frac_quiet) noexcept
	{
		if (exp_max) {
			if (frac_zero) return sign ? (1U << 0) : (1U << 7); // -inf / +inf
			return frac_quiet ? (1U << 9) : (1U << 8);          // qNaN / sNaN
		}
		if (exp_zero) {
			if (frac_zero) return sign ? (1U << 3) : (1U << 4); // -0.0 / +0.0
			return sign ? (1U << 2) : (1U << 5);                // -subnormal / +subnormal
		}
		return sign ? (1U << 1) : (1U << 6);                    // -normal / +normal
	}
	static inline uint32_t rv_fclass32(uint32_t bits) noexcept {
		const uint32_t exponent = (bits >> 23) & 0xff;
		const uint32_t fraction = bits & 0x7fffff;
		return rv_fclassify(bits >> 31, exponent == 0xff, exponent == 0,
			fraction == 0, (fraction & 0x400000) != 0);
	}
	static inline uint32_t rv_fclass64(uint64_t bits) noexcept {
		const uint64_t exponent = (bits >> 52) & 0x7ff;
		const uint64_t fraction = bits & 0xfffffffffffffULL;
		return rv_fclassify(bits >> 63, exponent == 0x7ff, exponent == 0,
			fraction == 0, (fraction & 0x8000000000000ULL) != 0);
	}
} // riscv
