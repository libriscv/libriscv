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
	};
}
