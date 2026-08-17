#pragma once
#include <cstdint>

namespace riscv
{
	/**
	 * binary16 conversion, shared by scalar Zfhmin and vector Zvfhmin.
	 *
	 * Both extensions mandate the *conversions* to and from half precision
	 * with no arithmetic. f16 to f32 always works, and f32 to f16 rounds to nearest-even.
	 */
	namespace fp16
	{
		inline float to_f32(const uint16_t h) noexcept
		{
			const uint32_t sign = uint32_t(h & 0x8000) << 16;
			uint32_t exp  = (h >> 10) & 0x1F;
			uint32_t sig  = h & 0x3FF;
			uint32_t bits;
			if (exp == 0x1F) {
				// Infinity and NaN keep their payload, shifted into place.
				bits = sign | 0x7F800000u | (sig << 13);
			} else if (exp == 0) {
				if (sig == 0) {
					bits = sign;
				} else {
					// Subnormal: renormalise into f32's much wider range.
					exp = 1;
					while ((sig & 0x400) == 0) { sig <<= 1; exp--; }
					sig &= 0x3FF;
					bits = sign | ((exp + (127 - 15)) << 23) | (sig << 13);
				}
			} else {
				bits = sign | ((exp + (127 - 15)) << 23) | (sig << 13);
			}
			float out;
			__builtin_memcpy(&out, &bits, sizeof(out));
			return out;
		}

		inline uint16_t from_f32(const float value) noexcept
		{
			uint32_t bits;
			__builtin_memcpy(&bits, &value, sizeof(bits));
			const uint16_t sign = uint16_t((bits >> 16) & 0x8000);
			const int32_t  exp  = int32_t((bits >> 23) & 0xFF) - 127 + 15;
			const uint32_t sig  = bits & 0x7FFFFF;

			if (((bits >> 23) & 0xFF) == 0xFF) {
				// A NaN must stay a NaN even when its payload does not fit.
				if (sig != 0)
					return uint16_t(sign | 0x7E00);
				return uint16_t(sign | 0x7C00);
			}
			if (exp >= 0x1F)               // overflows to infinity
				return uint16_t(sign | 0x7C00);
			if (exp <= 0) {
				// Subnormal or zero: shift the hidden bit back in and round
				// to nearest-even at the new position.
				if (exp < -10)
					return sign;
				const uint32_t full = sig | 0x800000;
				const unsigned shift = unsigned(14 - exp);
				uint32_t out = full >> shift;
				const uint32_t rem = full & ((1u << shift) - 1);
				const uint32_t half = 1u << (shift - 1);
				if (rem > half || (rem == half && (out & 1)))
					out++;
				return uint16_t(sign | out);
			}
			uint32_t out = (uint32_t(exp) << 10) | (sig >> 13);
			const uint32_t rem = sig & 0x1FFF;
			if (rem > 0x1000 || (rem == 0x1000 && (out & 1)))
				out++;  // carrying into the exponent is exactly what we want
			return uint16_t(sign | out);
		}

		/**
		 * A double narrows in one step rather than two: going via f32 would
		 * round twice, and a value that lands exactly on an f32 tie can come
		 * out one ulp away from what a single correctly-rounded narrowing
		 * gives. Only subnormal and overflow need the wider exponent.
		 */
		inline uint16_t from_f64(const double value) noexcept
		{
			uint64_t bits;
			__builtin_memcpy(&bits, &value, sizeof(bits));
			const uint16_t sign = uint16_t((bits >> 48) & 0x8000);
			const int32_t  rawexp = int32_t((bits >> 52) & 0x7FF);
			const uint64_t sig  = bits & 0xFFFFFFFFFFFFFull;

			if (rawexp == 0x7FF) {
				if (sig != 0)
					return uint16_t(sign | 0x7E00);
				return uint16_t(sign | 0x7C00);
			}
			const int32_t exp = rawexp - 1023 + 15;
			// exp >= 31 means the magnitude is at least 2^16, which is past
			// the tie point above the largest finite half in every case.
			if (exp >= 0x1F)
				return uint16_t(sign | 0x7C00);
			if (rawexp == 0 && sig == 0)
				return sign;

			// Assemble the significand with its hidden bit and round to
			// nearest-even at bit 10, keeping every discarded bit in the
			// sticky so a tie is a tie only when it really is one.
			const uint64_t full = (rawexp == 0) ? sig : (sig | (1ull << 52));
			const int32_t  e    = (rawexp == 0) ? (1 - 1023 + 15) : exp;
			// Shift so that the result's bit 0 lands at position 0. For a
			// normal result that is 42 bits; subnormals shift further.
			int32_t shift = 42;
			if (e <= 0)
				shift += 1 - e;
			if (shift > 63)
				return sign;
			uint64_t out = full >> shift;
			const uint64_t rem  = full & ((1ull << shift) - 1);
			const uint64_t half = 1ull << (shift - 1);
			if (rem > half || (rem == half && (out & 1)))
				out++;
			if (e > 0) {
				// out now holds the hidden bit at position 10, which is
				// where the exponent field begins, so adding e - 1 biased
				// exponents lands the fields side by side, and a carry out
				// of the significand carries into the exponent by itself.
				out += uint64_t(e - 1) << 10;
				if (out >= 0x7C00)
					return uint16_t(sign | 0x7C00);
			}
			return uint16_t(sign | uint16_t(out));
		}

		inline double to_f64(const uint16_t h) noexcept
		{
			// Widening is exact in both steps, so the f32 route is safe.
			return double(to_f32(h));
		}
	}
}
