#pragma once
#include <cstdint>
#include <string>
#include "types.hpp"

namespace riscv
{
	struct RV32I {
		using instruction_t = Instruction<4>;

		static std::string to_string(const CPU<4>& cpu, instruction_format format, const instruction_t& instr);
	};
}
