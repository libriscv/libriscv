#pragma once
#include "common.hpp" // LIKELY
#include <cmath> // std::fma for spec-compliant FMA (rv32f_fmadd)
#include <type_traits>

#ifdef __APPLE__
#include "TargetConditionals.h" // TARGET_* macros
#endif

#ifdef __GNUG__
#define RISCV_NOINLINE __attribute__((noinline))
#define RISCV_UNREACHABLE() __builtin_unreachable()
#define RISCV_EXPORT __attribute__((visibility("default")))
#else
#define RISCV_NOINLINE    /* */
#define RISCV_UNREACHABLE()  /* */
#  ifdef _MSC_VER
#    define RISCV_EXPORT __declspec(dllexport)
#  else
#    define RISCV_EXPORT      /* */
#  endif
#endif

#ifdef RISCV_32I
#define INSTANTIATE_32_IF_ENABLED(x) template struct x<4>
#else
#define INSTANTIATE_32_IF_ENABLED(x) /* */
#endif

#ifdef RISCV_64I
#define INSTANTIATE_64_IF_ENABLED(x) template struct x<8>
#else
#define INSTANTIATE_64_IF_ENABLED(x) /* */
#endif

#ifdef RISCV_128I
#define INSTANTIATE_128_IF_ENABLED(x) template struct x<16>
#else
#define INSTANTIATE_128_IF_ENABLED(x) /* */
#endif

namespace riscv
{
	/**
	 * Signed DIV and REM both have two operands that C++ cannot divide by:
	 * zero, and -1 when the dividend is the most negative value (the result
	 * is unrepresentable, which is UB and traps on x86). Instead of testing
	 * for those separately, note that unsigned src2+1 is 1 for zero and 0 for
	 * -1, and >1 for every other divisor. So a single add and compare gets us
	 * onto the fast path, with both special cases folded into the cold path.
	 *
	 * T is the unsigned register type (or the unsigned 32-bit operand type of
	 * the *W instructions).
	*/
	template <typename T>
	static inline T rv_div(T src1, T src2) noexcept
	{
		using S = typename std::make_signed<T>::type;
		if (LIKELY(T(src2 + 1) > T(1)))
			return T(S(src1) / S(src2));
		// Division by zero is not an exception: rd = -1
		// Signed overflow is not an exception either: rd = the dividend, and
		// negating the most negative value wraps back to it, so 0-src1 is
		// correct for every dividend when the divisor is -1.
		return (src2 == 0) ? T(-1) : T(T(0) - src1);
	}

	template <typename T>
	static inline T rv_rem(T src1, T src2) noexcept
	{
		using S = typename std::make_signed<T>::type;
		if (LIKELY(T(src2 + 1) > T(1)))
			return T(S(src1) % S(src2));
		// Remainder by zero is not an exception: rd = the dividend
		// Signed overflow is not an exception either: rd = 0, which is also
		// the remainder of every other dividend when the divisor is -1.
		return (src2 == 0) ? src1 : T(0);
	}
} // riscv

#ifndef ANTI_FINGERPRINTING_MASK_MICROS
#define ANTI_FINGERPRINTING_MASK_MICROS()  ~0x3LL
#endif
#ifndef ANTI_FINGERPRINTING_MASK_NANOS
#define ANTI_FINGERPRINTING_MASK_NANOS()   ~0x3FFFLL
#endif
