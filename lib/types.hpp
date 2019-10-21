#pragma once
#include <cstdint>
#include <type_traits>

namespace riscv
{
	struct RV32I;
	struct RV64I;

	template <int N>
	using address_type = typename std::conditional<(N == 4), uint32_t, uint64_t>::type;

	template <int N>
	using isa_type = typename std::conditional<(N == 4), RV32I, RV64I>::type;

	using interrupt_t = uint8_t;

	enum exceptions
	{
		ILLEGAL_OPCODE,
		MISALIGNED_INSTRUCTION,
		UNIMPLEMENTED_INSTRUCTION
	};
}
