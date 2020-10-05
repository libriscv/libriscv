#pragma once
#include <array>
#include "common.hpp"
#include "types.hpp"

namespace riscv {

template <int W>
struct DecoderCache
{
	using handler = instruction_handler<W>;
	// we are making room for the maximum amount of
	// compressed instructions, which are 16-bits
	static constexpr size_t DIVISOR = (compressed_enabled) ? 2 : 4;

	inline auto& get(size_t idx) noexcept {
		return cache[idx];
	}

	inline auto* get_base() noexcept {
		return &cache[0];
	}

	std::array<handler, PageSize / DIVISOR> cache = {nullptr};
};

}
