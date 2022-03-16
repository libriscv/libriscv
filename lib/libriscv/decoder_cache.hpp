#pragma once
#include <array>
#include "common.hpp"
#include "types.hpp"

namespace riscv {

template <int W>
struct DecoderData {
#ifdef RISCV_DEBUG
	using Handler = Instruction<W>;
#else
	using Handler = instruction_handler<W>;
#endif
	Handler handler;
};

template <int W>
struct DecoderCache
{
	static constexpr size_t DIVISOR = (compressed_enabled) ? 2 : 4;

	inline auto& get(size_t idx) noexcept {
		return cache[idx];
	}

	inline auto* get_base() noexcept {
		return &cache[0];
	}

	static void convert(const Instruction<W>& insn, DecoderData<W>& entry) {
#ifdef RISCV_DEBUG
		entry.handler = insn;
#else
		entry.handler = insn.handler;
#endif
	}
	static bool isset(const DecoderData<W>& entry) {
#ifdef RISCV_DEBUG
		return entry.handler.handler != nullptr;
#else
		return entry.handler != nullptr;
#endif
	}

	std::array<DecoderData<W>, PageSize / DIVISOR> cache = {};
};

}
