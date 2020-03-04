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
		constexpr void jump(address_t);

		auto& registers() { return this->m_regs; }
		const auto& registers() const { return this->m_regs; }

		auto& reg(uint32_t idx) { return registers().get(idx); }
		const auto& reg(uint32_t idx) const { return registers().get(idx); }
		auto& cireg(uint16_t idx) { return registers().get(idx + 0x8); }
		auto& ciflp(uint16_t idx) { return registers().getfl(idx + 0x8); }

		auto& machine() noexcept { return this->m_machine; }
		const auto& machine() const noexcept { return this->m_machine; }

		auto& atomics() noexcept { return this->m_atomics; }
		const auto& atomics() const noexcept { return this->m_atomics; }

		static void trigger_exception(interrupt_t) COLD_PATH();

#ifdef RISCV_DEBUG
		// debugging
	    void breakpoint(address_t address, breakpoint_t = default_pausepoint);
	    auto& breakpoints() { return this->m_breakpoints; }
	    void break_on_steps(int steps);
	    void break_checks();
		static void default_pausepoint(CPU&);
#endif

		// serializes all the machine state + a tiny header to @vec
		void serialize_to(std::vector<uint8_t>& vec);
		// returns the machine to a previously stored state
		void deserialize_from(const std::vector<uint8_t>&, const SerializedMachine<W>&);

		CPU(Machine<W>&);
	private:
		Registers<W> m_regs;
		AtomicMemory<W> m_atomics;

		inline format_t read_instruction(address_t);
#ifndef RISCV_DEBUG
		void execute(format_t);
#else
		const instruction_t& decode(format_t) const;
#endif

		Machine<W>& m_machine;
		int64_t     m_current_page = -1;
		const Page* m_page_pointer = nullptr;
		inline void change_page(address_t address);

#ifdef RISCV_DEBUG
		// instruction step & breakpoints
	    mutable int32_t m_break_steps = 0;
	    mutable int32_t m_break_steps_cnt = 0;
	    std::map<address_t, breakpoint_t> m_breakpoints;
		bool break_time() const;
		friend struct Machine<W>;
#endif
		static_assert((W == 4 || W == 8), "Must be either 4-byte or 8-byte ISA");
	};

#include "cpu_inline.hpp"
}
