#pragma once
#include <array>
#include <cstdint>
#include <string>
#include "types.hpp"
#include "rv32c.hpp"

namespace riscv
{
	struct RV64I {
		using address_t     = uint64_t;
		using format_t      = rv32i_instruction;
		using compressed_t  = rv32c_instruction;
		using instruction_t = Instruction<8>;
		using register_t    = uint64_t;

		static std::string to_string(CPU<8>& cpu, format_t format, const instruction_t& instr);

		static inline uint64_t SRA(bool is_signed, uint64_t shifts, uint64_t value)
		{
			const uint64_t sign_bits = -is_signed ^ 0x0;
			const uint64_t sign_shifted = sign_bits << (64 - shifts);
			return (value >> shifts) | sign_shifted;
		}
	};
}
