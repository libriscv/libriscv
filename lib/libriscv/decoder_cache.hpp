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
#ifdef RISCV_FAST_SIMULATOR
	uint32_t instr;
	uint16_t idxend;
	// XXX: Original_opcode is only relevant during decoding.
	union {
		uint16_t original_opcode;
		struct {
			uint8_t opcode_length;
			uint8_t instr_count;
		};
	};

	void execute(CPU<W>& cpu) {
		this->handler(cpu, instruction_format{this->instr});
	}
#endif // RISCV_FAST_SIMULATOR

	template <typename... Args>
	void execute(CPU<W>& cpu, Args... args) {
#ifdef RISCV_DEBUG
		this->handler.handler(cpu, args...);
#else
		this->handler(cpu, args...);
#endif
	}
	bool isset() const noexcept {
#ifdef RISCV_DEBUG
		return handler.handler != nullptr;
#else
		return handler != nullptr;
#endif
	}
	void set_handler(Instruction<W> insn) noexcept {
#ifdef RISCV_DEBUG
		this->handler = insn;
#else
		this->handler = insn.handler;
#endif
	}
	void set_insn_handler(instruction_handler<W> ih) noexcept {
		this->handler = ih;
	}

private:
	size_t opcode_for(Handler new_handler) const {
		for (size_t i = 1; i < handlers.size(); i++) {
			if (handlers[i] == new_handler)
				return i;
			else if (handlers[i] == nullptr) {
				handlers[i] = new_handler;
				return i;
			}
		}
		//throw std::runtime_error("Not enough instruction handler space");
	}
	static std::array<Handler, 256> handlers;
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

	std::array<DecoderData<W>, PageSize / DIVISOR> cache = {};
};

}
