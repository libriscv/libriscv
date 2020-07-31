#pragma once
#include <array>
#include "types.hpp"

namespace riscv {

template <size_t PageSize>
union DecoderCache
{
	template <int W>
	using handler = instruction_handler<W>;

	template <int W>
	inline auto& get(size_t idx) noexcept {
		if constexpr (W == 4) {
			return cache32[idx];
		} else {
			return cache64[idx];
		}
	}

	template <int W>
	inline auto* get_base() noexcept {
		if constexpr (W == 4) {
			return &cache32[0];
		} else {
			return &cache64[0];
		}
	}

	// we are making room for the maximum amount of
	// compressed instructions, which are 16-bits
	static constexpr size_t DIVISOR = (compressed_enabled) ? 2 : 4;

	std::array<handler<4>, PageSize / DIVISOR> cache32 = {nullptr};
	std::array<handler<8>, PageSize / DIVISOR> cache64;
};

}
