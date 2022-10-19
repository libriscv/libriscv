#pragma once
#include <cstdint>
#include <string>
#include "types.hpp"

namespace riscv
{
	struct RV64I {
		using instruction_t = Instruction<8>;

		static std::string to_string(const CPU<8>& cpu, instruction_format format, const instruction_t& instr);
	};
}
