#pragma once
#include <cstdint>
#include <string>
#include "types.hpp"

namespace riscv
{
	struct RV64I {
		using instruction_t = Instruction<8>;

		static std::string to_string(const CPU<8>& cpu, instruction_format format, const instruction_t& instr);

		static inline uint64_t SRA(bool is_signed, uint32_t shifts, uint64_t value)
		{
			const uint64_t sign_bits = -is_signed ^ 0x0;
			const uint64_t sign_shifted = sign_bits << (64 - shifts);
			return (value >> shifts) | sign_shifted;
		}

		// https://stackoverflow.com/questions/28868367/getting-the-high-part-of-64-bit-integer-multiplication
		// As written by catid
		static inline uint64_t MUL128(
			uint64_t& r_hi,
			const uint64_t x,
			const uint64_t y)
		{
		    const uint64_t x0 = (uint32_t)x, x1 = x >> 32;
		    const uint64_t y0 = (uint32_t)y, y1 = y >> 32;
		    const uint64_t p11 = x1 * y1, p01 = x0 * y1;
		    const uint64_t p10 = x1 * y0, p00 = x0 * y0;
		    /*
		        This is implementing schoolbook multiplication:

		                x1 x0
		        X       y1 y0
		        -------------
		                   00  LOW PART
		        -------------
		                00
		             10 10     MIDDLE PART
		        +       01
		        -------------
		             01
		        + 11 11        HIGH PART
		        -------------
		    */

		    // 64-bit product + two 32-bit values
		    const uint64_t middle = p10 + (p00 >> 32) + (uint32_t)p01;

		    /*
		        Proof that 64-bit products can accumulate two more 32-bit values
		        without overflowing:

		        Max 32-bit value is 2^32 - 1.
		        PSum = (2^32-1) * (2^32-1) + (2^32-1) + (2^32-1)
		             = 2^64 - 2^32 - 2^32 + 1 + 2^32 - 1 + 2^32 - 1
		             = 2^64 - 1
		        Therefore it cannot overflow regardless of input.
		    */

		    // 64-bit product + two 32-bit values
		    r_hi = p11 + (middle >> 32) + (p01 >> 32);

		    // Add LOW PART and lower half of MIDDLE PART
		    return (middle << 32) | (uint32_t)p00;
		}
	};
}
