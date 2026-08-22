#pragma once
#include "common.hpp" // LIKELY
#include <cmath> // std::fma for spec-compliant FMA (rv32f_fmadd)
#include <cstdint>
#include <type_traits>
#ifdef _MSC_VER
#include <intrin.h> // __umulh
#endif

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
	// Signed DIV/REM with RISC-V semantics (no trap on div-by-zero or overflow).
	// Fast path: unsigned (src2+1) > 1 excludes both zero and -1 in one compare.
	template <typename T>
	static inline T rv_div(T src1, T src2) noexcept
	{
		using S = typename std::make_signed<T>::type;
		if (LIKELY(T(src2 + 1) > T(1)))
			return T(S(src1) / S(src2));
		// div-by-zero => -1; signed overflow (INT_MIN / -1) => INT_MIN.
		return (src2 == 0) ? T(-1) : T(T(0) - src1);
	}

	template <typename T>
	static inline T rv_rem(T src1, T src2) noexcept
	{
		using S = typename std::make_signed<T>::type;
		if (LIKELY(T(src2 + 1) > T(1)))
			return T(S(src1) % S(src2));
		// rem-by-zero => dividend; signed overflow => 0.
		return (src2 == 0) ? src1 : T(0);
	}

	// Upper 64 bits of a 64x64 multiply (MULH/MULHU/MULHSU and RVV vmulh).
	// __int128 path is exact; the 32-bit partial-product fallback is unsigned-only.
#if defined(_MSC_VER)
	static inline uint64_t mulhu64(uint64_t a, uint64_t b) noexcept {
		return __umulh(a, b);
	}
	static inline uint64_t mulhi64(uint64_t a, uint64_t b) noexcept {
		return (uint64_t)__mulh((int64_t)a, (int64_t)b);
	}
	static inline uint64_t mulhsu64(uint64_t a, uint64_t b) noexcept {
		return __umulh(a, b);
	}
#elif defined(__SIZEOF_INT128__)
	static inline uint64_t mulhu64(uint64_t a, uint64_t b) noexcept {
		return uint64_t((__uint128_t(a) * __uint128_t(b)) >> 64u);
	}
	static inline uint64_t mulhi64(uint64_t a, uint64_t b) noexcept {
		return uint64_t((__int128_t(int64_t(a)) * __int128_t(int64_t(b))) >> 64u);
	}
	static inline uint64_t mulhsu64(uint64_t a, uint64_t b) noexcept {
		return uint64_t((__int128_t(int64_t(a)) * __int128_t(b)) >> 64u);
	}
#else
	// Fallback: 32-bit partial products (stackoverflow.com/q/28868367, catid).
	static inline uint64_t MUL128(
		uint64_t* r_hi,
		const uint64_t x,
		const uint64_t y)
	{
		const uint64_t x0 = (uint32_t)x, x1 = x >> 32;
		const uint64_t y0 = (uint32_t)y, y1 = y >> 32;
		const uint64_t p11 = x1 * y1, p01 = x0 * y1;
		const uint64_t p10 = x1 * y0, p00 = x0 * y0;

		const uint64_t middle = p10 + (p00 >> 32) + (uint32_t)p01;
		*r_hi = p11 + (middle >> 32) + (p01 >> 32);
		return (middle << 32) | (uint32_t)p00;
	}
	static inline uint64_t mulhu64(uint64_t a, uint64_t b) noexcept {
		uint64_t hi;
		MUL128(&hi, a, b);
		return hi;
	}
	static inline uint64_t mulhi64(uint64_t a, uint64_t b) noexcept {
		return mulhu64(a, b);
	}
	static inline uint64_t mulhsu64(uint64_t a, uint64_t b) noexcept {
		return mulhu64(a, b);
	}
#endif
} // riscv


#ifndef ANTI_FINGERPRINTING_MASK_MICROS
#define ANTI_FINGERPRINTING_MASK_MICROS()  ~0x3LL
#endif
#ifndef ANTI_FINGERPRINTING_MASK_NANOS
#define ANTI_FINGERPRINTING_MASK_NANOS()   ~0x3FFFLL
#endif
