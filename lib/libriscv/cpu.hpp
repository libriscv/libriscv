#pragma once
#include "common.hpp"
#include "page.hpp"
#include "registers.hpp"
#ifdef RISCV_EXT_ATOMICS
#include "rva.hpp"
#endif
#include "util/function.hpp"
#ifdef RISCV_DEBUG
#include <map>
#endif
#include <vector>

namespace riscv
{
	template<int W> struct Machine;

	template<int W>
	struct CPU
	{
		using address_t = address_type<W>;          // one unsigned memory address
		using format_t  = instruction_format; // one machine instruction
		using breakpoint_t = Function<void(CPU<W>&)>; // machine instruction
		using instruction_t = Instruction<W>;

		void simulate();
		void reset();
		void reset_stack_pointer() noexcept;

		address_t pc() const noexcept { return registers().pc; }
		void increment_pc(int delta) { registers().pc += delta; }
		constexpr void jump(address_t);

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
		template <typename Type>
		void amo(format_t, void(*op)(CPU&, register_type<W>&, uint32_t));
#endif
		__attribute__((noreturn))
		static void trigger_exception(interrupt_t, address_t = 0) COLD_PATH();

#ifdef RISCV_DEBUG
		// debugging
	    void breakpoint(address_t address, breakpoint_t = default_pausepoint);
	    auto& breakpoints() { return this->m_breakpoints; }
	    void break_on_steps(int steps);
	    void break_checks();
		static void default_pausepoint(CPU&);
#endif
		const instruction_t& decode(format_t) const;
		void initialize_exec_segs(const uint8_t* data, address_t begin, address_t end)
			{ m_exec_data = data; m_exec_begin = begin; m_exec_end = end; }
		const uint8_t* exec_seg_data() const { return m_exec_data; }

		// serializes all the machine state + a tiny header to @vec
		void serialize_to(std::vector<uint8_t>& vec);
		// returns the machine to a previously stored state
		void deserialize_from(const std::vector<uint8_t>&, const SerializedMachine<W>&);

		// instruction fusing (icache only)
		using instr_pair = std::pair<instruction_handler<W>&, format_t&>;
		void try_translate(address_t pc, std::vector<instr_pair>&) const;
		bool try_fuse(instr_pair i1, instr_pair i2) const;

		CPU(Machine<W>&);
	private:
		Registers<W> m_regs;
		Machine<W>&  m_machine;

		format_t read_next_instruction();
		format_t read_next_instruction_slowpath() COLD_PATH();
		void execute(format_t);
		void emit(std::string& code, const std::string& symb, instr_pair* blk, size_t len, const TransInfo<W>&) const;

		// ELF programs linear .text segment
		const uint8_t* m_exec_data = nullptr;
		address_t m_exec_begin = 0;
		address_t m_exec_end   = 0;

		// Page cache for execution on virtual memory
		CachedPage<W, const Page> m_cache;

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

#ifndef RISCV_TRANSLATION_DYLIB
#include "cpu_inline.hpp"
#endif
}
