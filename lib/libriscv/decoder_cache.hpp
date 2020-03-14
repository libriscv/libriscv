#pragma once
#include <array>
#include "types.hpp"
#include "page.hpp"
#include "util/delegate.hpp"

namespace riscv {

union DecoderCache
{
	using handler32 = Instruction<4>::handler_t;
	using handler64 = Instruction<8>::handler_t;
#ifdef RISCV_EXT_COMPRESSED
	// we are making room for the maximum amount of
	// compressed instructions, which are 16-bits
	static constexpr size_t DIVISOR = 2;
#else
	// all instructions are 32-bit
	static constexpr size_t DIVISOR = 4;
#endif

	std::array<handler32, PageData::SIZE / DIVISOR> cache32 = {nullptr};
	std::array<handler64, PageData::SIZE / DIVISOR> cache64;
};

}
