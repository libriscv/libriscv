#pragma once
#include "common.hpp"
#include "page.hpp"
#include "registers.hpp"
#include "rv32i.hpp"
#include "rv64i.hpp"
#include "rv32a.hpp"
#include "util/function.hpp"
#include <map>
#include <vector>

namespace riscv
{
	template<int W> struct Machine;

	template<int W>
	struct CPU
	{
		using address_t = address_type<W>;          // one unsigned memory address
		using isa_t     = isa_type<W>;              // 32- or 64-bit architecture
		using format_t  = typename isa_t::format_t; // one machine instruction
		using breakpoint_t = Function<void(CPU<W>&)>; // machine instruction
		using instruction_t = Instruction<W>;

		void simulate();
		void reset();
		void reset_stack_pointer() noexcept;

		address_t pc() const noexcept { return registers().pc; }
		constexpr void jump(address_t);

		uint64_t instruction_counter() const noexcept { return m_counter; }
		void     increment_counter(uint64_t val) noexcept { m_counter += val; }
		void     reset_instruction_counter() noexcept { m_counter = 0; }

		auto& registers() { return this->m_regs; }
		const auto& registers() const { return this->m_regs; }

		auto& reg(uint32_t idx) { return registers().get(idx); }
		const auto& reg(uint32_t idx) const { return registers().get(idx); }
		auto& cireg(uint16_t idx) { return registers().get(idx + 0x8); }
		auto& ciflp(uint16_t idx) { return registers().getfl(idx + 0x8); }

		auto& machine() noexcept { return this->m_machine; }
		const auto& machine() const noexcept { return this->m_machine; }

#ifdef RISCV_EXT_ATOMICS
		auto& atomics() noexcept { return this->m_atomics; }
		const auto& atomics() const noexcept { return this->m_atomics; }
#endif
		static void trigger_exception(interrupt_t) COLD_PATH();

#ifdef RISCV_DEBUG
		// debugging
	    void breakpoint(address_t address, breakpoint_t = default_pausepoint);
	    auto& breakpoints() { return this->m_breakpoints; }
	    void break_on_steps(int steps);
	    void break_checks();
		static void default_pausepoint(CPU&);
#endif
		const instruction_t& decode(format_t) const;

		// serializes all the machine state + a tiny header to @vec
		void serialize_to(std::vector<uint8_t>& vec);
		// returns the machine to a previously stored state
		void deserialize_from(const std::vector<uint8_t>&, const SerializedMachine<W>&);

		CPU(Machine<W>&);
	private:
		Registers<W> m_regs;
		uint64_t     m_counter = 0;

		inline format_t read_next_instruction();
		void execute(format_t);

		Machine<W>& m_machine;
		struct CachedPage {
			Page*   page = nullptr;
			int64_t pageno = 0;
		};
		CachedPage m_current_page;
#ifdef RISCV_PAGE_CACHE
		std::array<CachedPage, RISCV_PAGE_CACHE> m_page_cache = {};
		int m_cache_iterator = 0;
#endif
		inline void change_page(int pageno);
		inline void check_page(CachedPage&);

#ifdef RISCV_DEBUG
		// instruction step & breakpoints
	    mutable int32_t m_break_steps = 0;
	    mutable int32_t m_break_steps_cnt = 0;
	    std::map<address_t, breakpoint_t> m_breakpoints;
		bool break_time() const;
		friend struct Machine<W>;
#endif
#ifdef RISCV_EXT_ATOMICS
		AtomicMemory<W> m_atomics;
#endif
		static_assert((W == 4 || W == 8), "Must be either 4-byte or 8-byte ISA");
	};

#include "cpu_inline.hpp"
}
