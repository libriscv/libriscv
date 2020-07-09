#pragma once
#include <array>
#include "types.hpp"
#include "rv32i.hpp"
#include "rv64i.hpp"

namespace riscv {

template <size_t PageSize>
union DecoderCache
{
	using handler32 = Instruction<4>::handler_t;
	using handler64 = Instruction<8>::handler_t;

	// we are making room for the maximum amount of
	// compressed instructions, which are 16-bits
	static constexpr size_t DIVISOR = (compressed_enabled) ? 2 : 4;

	std::array<handler32, PageSize / DIVISOR> cache32 = {nullptr};
	std::array<handler64, PageSize / DIVISOR> cache64;
};

}
