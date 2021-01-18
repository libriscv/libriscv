#pragma once
#include <array>
#include "common.hpp"
#include "types.hpp"

namespace riscv {

template <int W>
struct DecoderCache
{
#ifdef RISCV_DEBUG
	using handler = Instruction<W>;
#else
	using handler = instruction_handler<W>;
#endif
	// we are making room for the maximum amount of
	// compressed instructions, which are 16-bits
	static constexpr size_t DIVISOR = (compressed_enabled) ? 2 : 4;

	inline auto& get(size_t idx) noexcept {
		return cache[idx];
	}

	inline auto* get_base() noexcept {
		return &cache[0];
	}

	static void convert(const Instruction<W>& insn, handler& entry) {
#ifdef RISCV_DEBUG
		entry = insn;
#else
		entry = insn.handler;
#endif
	}
	static bool isset(const handler& entry) {
#ifdef RISCV_DEBUG
		return entry.handler != nullptr;
#else
		return entry != nullptr;
#endif
	}

	std::array<handler, PageSize / DIVISOR> cache = {};
};

}
