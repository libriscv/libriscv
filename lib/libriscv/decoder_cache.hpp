#pragma once
#include <array>
#include "types.hpp"

namespace riscv {

template <size_t PageSize>
union DecoderCache
{
	using handler32 = instruction_handler<4>;
	using handler64 = instruction_handler<8>;

	template <int W>
	inline auto& get(size_t idx) noexcept {
		if constexpr (W == 4) {
			return cache32[idx];
		} else {
			return cache64[idx];
		}
	}

	// we are making room for the maximum amount of
	// compressed instructions, which are 16-bits
	static constexpr size_t DIVISOR = (compressed_enabled) ? 2 : 4;

	std::array<handler32, PageSize / DIVISOR> cache32 = {nullptr};
	std::array<handler64, PageSize / DIVISOR> cache64;
};

}
