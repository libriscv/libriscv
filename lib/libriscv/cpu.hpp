#pragma once
#include "types.hpp"
#include "registers.hpp"
#include "rv32i.hpp"
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

		void trigger_interrupt(interrupt_t intr);

		auto& registers() { return this->m_data.m_regs; }
		const auto& registers() const { return this->m_data.m_regs; }

		inline auto& reg(uint32_t idx) { return registers().get(idx); }
		inline auto& cireg(uint16_t idx) { return registers().get(idx + 0x8); }

		auto& machine() { return this->m_machine; }
		const auto& machine() const { return this->m_machine; }

		// debugging
	    void breakpoint(address_t address, breakpoint_t = default_pausepoint);
	    auto& breakpoints() { return this->m_breakpoints; }
	    void break_on_steps(int steps);
	    void break_now() { this->m_break = true; }
	    void break_checks();
	    bool is_breaking() const noexcept { return this->m_break; }
	    static void print_and_pause(CPU&);
		static void default_pausepoint(CPU&);

		CPU(Machine<W>&);
	private:
		struct {
			Registers<W> m_regs;
			std::vector<interrupt_t> interrupt_queue;
			bool interrupt_master_enable = true;
		} m_data;

		inline void execute();
		format_t read_instruction(address_t);
		const instruction_t& decode(format_t) const;

		void handle_interrupts();
		void execute_interrupt(interrupt_t intr);

		Machine<W>& m_machine;

		// debugging
	    bool m_break = false;
	    mutable int16_t m_break_steps = 0;
	    mutable int16_t m_break_steps_cnt = 0;
	    std::map<address_t, breakpoint_t> m_breakpoints;
		bool break_time() const;

		static_assert((W == 4 || W == 8), "Must be either 4-byte or 8-byte ISA");
	};

#include "cpu_inline.hpp"
}
