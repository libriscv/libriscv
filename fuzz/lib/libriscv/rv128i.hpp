#pragma once
#include <cstdint>
#include <string>
#include "types.hpp"

namespace riscv
{
	struct RV128I {
		using instruction_t = Instruction<16>;

		static __uint128_t SRA(bool is_signed, uint32_t shifts, __uint128_t value)
		{
			const __uint128_t sign_bits = -is_signed ^ 0x0;
			const __uint128_t sign_shifted = sign_bits << (128 - shifts);
			return (value >> shifts) | sign_shifted;
		}

		static size_t to_hex(char* buffer, size_t len, __uint128_t value)
		{
			if (len < 32) return 0;
			len = 8; /* At least print 8 hex digits */
			static constexpr char lut[] = "0123456789ABCDEF";
			for (unsigned i = 0; i < 16 - len / 2; i++) {
				if ((value >> ((15-i) * 8)) & 0xFF) {
					len = 32 - i * 2;
					break;
				}
			}
			const size_t max = len / 2;
			for (unsigned i = 0; i < max; i++) {
				buffer[i*2 + 0] = lut[(value >> ((max-1-i) * 8 + 4)) & 0xF];
				buffer[i*2 + 1] = lut[(value >> ((max-1-i) * 8 + 0)) & 0xF];
			}
			return len;
		}

		static std::string to_string(const CPU<16>& cpu, instruction_format format, const instruction_t& instr);
	};
}
