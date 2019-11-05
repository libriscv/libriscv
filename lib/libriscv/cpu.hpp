#pragma once
#include "common.hpp"
#include "page.hpp"
#include "types.hpp"
#include "registers.hpp"
#include "rv32i.hpp"
#include "rv32a.hpp"
#include "util/delegate.hpp"
#include <map>
#include <tuple>
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
		using breakpoint_t = delegate<void(CPU<W>&)>; // machine instruction
		using instruction_t = Instruction<W>;

		void simulate();
		void reset();

		address_t pc() const noexcept { return registers().pc; }
		void jump(address_t);

		void trigger_exception(interrupt_t intr);

		auto& registers() { return this->m_data.m_regs; }
		const auto& registers() const { return this->m_data.m_regs; }

		inline auto& reg(uint32_t idx) { return registers().get(idx); }
		inline const auto& reg(uint32_t idx) const { return registers().get(idx); }
		inline auto& cireg(uint16_t idx) { return registers().get(idx + 0x8); }

		auto& machine() noexcept { return this->m_machine; }
		const auto& machine() const noexcept { return this->m_machine; }

		auto& atomics() noexcept { return this->m_atomics; }
		const auto& atomics() const noexcept { return this->m_atomics; }

#ifdef RISCV_DEBUG
		// debugging
	    void breakpoint(address_t address, breakpoint_t = default_pausepoint);
	    auto& breakpoints() { return this->m_breakpoints; }
	    void break_on_steps(int steps);
	    void break_now() { this->m_break = true; }
	    void break_checks();
	    bool is_breaking() const noexcept { return this->m_break; }
		static void default_pausepoint(CPU&);
#endif
		CPU(Machine<W>&);
	private:
		struct {
			Registers<W> m_regs;
		} m_data;
		AtomicMemory<W> m_atomics;

		inline void execute();
		inline format_t read_instruction(address_t);
#ifndef RISCV_DEBUG
		void execute(format_t);
#else
		const instruction_t& decode(format_t) const;
#endif

		Machine<W>& m_machine;
		int64_t     m_current_page = -1;
		const Page* m_page_pointer = nullptr;

#ifdef RISCV_DEBUG
		// instruction step & breakpoints
	    bool m_break = false;
	    mutable int32_t m_break_steps = 0;
	    mutable int32_t m_break_steps_cnt = 0;
	    std::map<address_t, breakpoint_t> m_breakpoints;
		bool break_time() const;
#endif
		static_assert((W == 4 || W == 8), "Must be either 4-byte or 8-byte ISA");
		friend struct Machine<W>;
	};

#include "cpu_inline.hpp"
}
