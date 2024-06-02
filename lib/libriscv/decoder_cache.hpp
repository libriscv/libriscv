#pragma once
#include "common.hpp"
#include "types.hpp"
#include <unordered_map>
#include <vector>

namespace riscv {

template <int W>
struct DecoderData {
	using Handler = instruction_handler<W>;

	uint8_t  m_bytecode;
	uint8_t  m_handler;
#ifdef RISCV_EXT_COMPRESSED
	uint16_t idxend  : 8;
	uint16_t icount  : 8;
#else
	uint16_t idxend;
#endif

	uint32_t instr;

	template <typename... Args>
	void execute(CPU<W>& cpu, Args... args) const {
		get_handler()(cpu, args...);
	}
	bool isset() const noexcept {
		return m_handler != 0x0;
	}
	void set_handler(Instruction<W> insn) noexcept {
		this->set_insn_handler(insn.handler);
	}

	// Switch-based and threaded simulation uses bytecodes.
	RISCV_ALWAYS_INLINE
	auto get_bytecode() const noexcept {
		return this->m_bytecode;
	}
	void set_bytecode(uint16_t num) noexcept {
		this->m_bytecode = num;
	}

	// Some simulation modes use function pointers
	// Eg. simulate_precise() and simulate_fastsim().
	RISCV_ALWAYS_INLINE
	Handler get_handler() const noexcept {
		return this->instr_handlers[m_handler];
	}
	void set_insn_handler(instruction_handler<W> ih) noexcept {
		this->m_handler = handler_index_for(ih);
	}

	RISCV_ALWAYS_INLINE
	auto block_bytes() const noexcept {
		return idxend * (compressed_enabled ? 2 : 4);
	}
	RISCV_ALWAYS_INLINE
	auto instruction_count() const noexcept {
#ifdef RISCV_EXT_COMPRESSED
		return idxend + 1 - icount;
#else
		return idxend + 1;
#endif
	}

	static size_t handler_index_for(Handler new_handler);
	static Handler* get_handlers() noexcept {
		return &instr_handlers[0];
	}
private:
	static inline std::array<Handler, 256> instr_handlers;
	static inline std::size_t handler_count = 0;
	static inline std::unordered_map<Handler, size_t> handler_cache;
};

template <int W>
struct alignas(64) DecoderCache
{
	static constexpr size_t DIVISOR = (compressed_enabled) ? 2 : 4;
	static constexpr unsigned SHIFT = (compressed_enabled) ? 1 : 2;

	inline auto& get(size_t idx) noexcept {
		return cache[idx];
	}

	inline auto* get_base() noexcept {
		return &cache[0];
	}

	std::array<DecoderData<W>, PageSize / DIVISOR> cache = {};
};

}
