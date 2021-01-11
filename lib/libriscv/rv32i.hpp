#pragma once
#include <cstdint>
#include <string>
#include "types.hpp"

namespace riscv
{
	union rv32c_instruction;
	union rv32i_instruction;

	struct RV32I {
		using instruction_t = Instruction<4>;

		static std::string to_string(CPU<4>& cpu, instruction_format format, const instruction_t& instr);

		static inline uint32_t SRA(bool is_signed, uint32_t shifts, uint32_t value)
		{
			const uint32_t sign_bits = -is_signed ^ 0x0;
			const uint32_t sign_shifted = sign_bits << (32 - shifts);
			return (value >> shifts) | sign_shifted;
		}
	};
}
