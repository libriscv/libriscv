#include "machine.hpp"
#include "decoder_cache.hpp"
#include "riscvbase.hpp"
#include "rv32i_instr.hpp"
#include "rv32i.hpp"
#include "rv64i.hpp"
#include "rv128i.hpp"

namespace riscv
{
	// Instructions may be unaligned with C-extension
	// On amd64 we take the cost, because it's faster
	union UnderAlign32 {
		uint16_t data[2];
		operator uint32_t() {
			return data[0] | uint32_t(data[1]) << 16;
		}
	};

	template <int W>
	CPU<W>::CPU(Machine<W>& machine, unsigned cpu_id, const Machine<W>& other)
		: m_machine { machine }, m_cpuid { cpu_id }
	{
		this->m_exec = other.cpu.m_exec;
		// Copy all registers except vectors
		// Users can still copy vector registers by assigning to registers().rvv().
		this->registers().copy_from(Registers<W>::Options::NoVectors, other.cpu.registers());
#ifdef RISCV_EXT_ATOMICS
		this->m_atomics = other.cpu.m_atomics;
#endif
	}
	template <int W>
	void CPU<W>::reset()
	{
		this->m_regs = {};
		this->reset_stack_pointer();
		// We can't jump if there's been no ELF loader
		if (!machine().memory.binary().empty()) {
			const auto initial_pc = machine().memory.start_address();
			// Validate that the initial PC is executable.
			// Inbound jumps feature does not allow other execute areas.
			// When execute-only is active, there is no reachable execute pages.
			const auto& page =
				machine().memory.get_exec_pageno(initial_pc / riscv::Page::size());
			if (UNLIKELY(!page.attr.exec)) {
				trigger_exception(EXECUTION_SPACE_PROTECTION_FAULT, initial_pc);
			}
			// This function will (at most) validate the execute segment
			this->jump(initial_pc);
		}
		// reset the page cache
		this->m_cache = {};
	}

	template <int W>
	void CPU<W>::init_execute_area(const void* vdata, address_t begin, address_t vlength)
	{
		if (vlength < 4)
			trigger_exception(EXECUTION_SPACE_PROTECTION_FAULT, begin);

		this->set_execute_segment(
			&machine().memory.create_execute_segment(
				{}, vdata, begin, vlength));
	} // CPU::init_execute_area

	template<int W> __attribute__((noinline))
	void CPU<W>::next_execute_segment()
	{
		// Find previously decoded execute segment
		this->m_exec = machine().memory.exec_segment_for(this->pc());
		if (this->m_exec != nullptr)
			return;

		auto base_pageno = this->pc() / Page::size() + 1;
		auto end_pageno  = base_pageno;
		// Find the earliest execute page in new segment
		while (base_pageno > 0) {
			const auto& page =
				machine().memory.get_pageno(base_pageno-1);
			if (page.attr.exec) {
				base_pageno -= 1;
			} else break;
		}
		// Check that the first page is executable, otherwise give error.
		if (base_pageno == end_pageno) {
			trigger_exception(EXECUTION_SPACE_PROTECTION_FAULT, this->pc());
		}

		// Find the last execute page in segment
		while (end_pageno != 0) {
			const auto& page =
				machine().memory.get_pageno(end_pageno);
			if (page.attr.exec) {
				end_pageno += 1;
			} else break;
		}

		const size_t n_pages = end_pageno - base_pageno;
		std::unique_ptr<uint8_t[]> area (new uint8_t[n_pages * Page::size()]);
		// Copy from each individual page
		for (address_t p = base_pageno; p < end_pageno; p++) {
			auto& page = machine().memory.get_exec_pageno(p);
			const size_t offset = (p - base_pageno) * Page::size();
			std::memcpy(area.get() + offset, page.data(), Page::size());
		}
		// Decode and store it for later
		this->init_execute_area(area.get(), base_pageno * Page::size(), n_pages * Page::size());
	} // CPU::next_execute_segment

	template <int W> __attribute__((noinline)) RISCV_INTERNAL
	typename CPU<W>::format_t CPU<W>::read_next_instruction_slowpath() const
	{
		// Fallback: Read directly from page memory
		const auto pageno = this->pc() >> Page::SHIFT;
		// Page cache
		auto& entry = this->m_cache;
		if (entry.pageno != pageno || entry.page == nullptr) {
			auto e = decltype(m_cache){pageno, &machine().memory.get_exec_pageno(pageno)};
			if (UNLIKELY(!e.page->attr.exec)) {
				trigger_exception(EXECUTION_SPACE_PROTECTION_FAULT, this->pc());
			}
			// delay setting entry until we know it's good!
			entry = e;
		}
		const auto& page = *entry.page;
		const auto offset = this->pc() & (Page::size()-1);
		format_t instruction;

		if (LIKELY(offset <= Page::size()-4)) {
			instruction.whole = uint32_t(*(UnderAlign32 *)(page.data() + offset));
			return instruction;
		}
		// It's not possible to jump to a misaligned address,
		// so there is necessarily 16-bit left of the page now.
		instruction.whole = *(uint16_t*) (page.data() + offset);

		// If it's a 32-bit instruction at a page border, we need
		// to get the next page, and then read the upper half
		if (UNLIKELY(instruction.is_long()))
		{
			const auto& page = machine().memory.get_exec_pageno(pageno+1);
			instruction.half[1] = *(uint16_t*) page.data();
		}

		return instruction;
	}

	template <int W>
	bool CPU<W>::is_executable(address_t addr) const noexcept {
		if (m_exec) return m_exec->is_within(addr);
		return false;
	}

	template <int W>
	typename CPU<W>::format_t CPU<W>::read_next_instruction() const
	{
		if (LIKELY(this->is_executable(this->pc()))) {
			auto* exd = m_exec->exec_data(this->pc());
			return format_t { *(uint32_t*) exd };
		}

		return read_next_instruction_slowpath();
	}

	template<int W> __attribute__((hot, no_sanitize("undefined")))
	void CPU<W>::simulate_precise(uint64_t max)
	{
		// Decoded segments are always faster
		// So, always have at least the current segment
		if (m_exec == nullptr) {
			this->next_execute_segment();
		}
		auto* exec = this->m_exec;
		auto* exec_decoder = exec->decoder_cache();
		auto* exec_seg_data = exec->exec_data();

		// Calculate the instruction limit
		if (max != UINT64_MAX)
			machine().set_max_instructions(machine().instruction_counter() + max);
		else
			machine().set_max_instructions(UINT64_MAX);

		for (; machine().instruction_counter() < machine().max_instructions();
			machine().increment_counter(1)) {

			format_t instruction;

		if (LIKELY(exec->is_within(this->pc()))) {
			auto pc = this->pc();

			// Instructions may be unaligned with C-extension
			// On amd64 we take the cost, because it's faster
#    if defined(RISCV_EXT_COMPRESSED) && !defined(__x86_64__)
			instruction = format_t { *(UnderAlign32*) &exec_seg_data[pc] };
#    else  // aligned/unaligned loads
			instruction = format_t { *(uint32_t*) &exec_seg_data[pc] };
#    endif // aligned/unaligned loads

			// Retrieve handler directly from the instruction handler cache
			auto& cache_entry =
				exec_decoder[pc / DecoderCache<W>::DIVISOR];
			cache_entry.execute(*this, instruction);
		} else {
			// The slow path reads from execute pages
			instruction = read_next_instruction_slowpath();
			// decode & execute instruction directly
			this->execute(instruction);
		}

			// increment PC
			if constexpr (compressed_enabled)
				registers().pc += instruction.length();
			else
				registers().pc += 4;
		} // while not stopped

	} // CPU::simulate_precise

#ifndef RISCV_FAST_SIMULATOR
	template<int W> __attribute__((hot))
	void CPU<W>::simulate(uint64_t imax)
	{
		simulate_precise(imax);
	}

#else // RISCV_FAST_SIMULATOR

	// In fastsim mode the instruction counter becomes a register
	// the function, and we only update m_counter in Machine on exit
	// When binary translation is enabled we cannot do this optimization.
	template <int W>
	struct InstrCounter {
		InstrCounter(Machine<W>& m) : machine(m), m_counter{m.instruction_counter()} {}
		~InstrCounter() {
			if constexpr (!binary_translation_enabled)
				machine.set_instruction_counter(m_counter);
		}

		uint64_t value() const noexcept {
			if constexpr (binary_translation_enabled)
				return machine.instruction_counter();
			else
				return m_counter; }
		void set_counter(uint64_t value) {
			m_counter = value;
		}
		void increment_counter(uint64_t cnt) {
			if constexpr (binary_translation_enabled)
				machine.increment_counter(cnt);
			else
				m_counter += cnt;
		}
		bool overflowed() const noexcept {
			if constexpr (binary_translation_enabled)
				return machine.stopped();
			else
				return m_counter >= machine.max_instructions();
		}
	private:
		Machine<W>& machine;
		uint64_t m_counter;
	};

	template<int W> __attribute__((hot))
	void CPU<W>::simulate(uint64_t imax)
	{
		// If we start outside of the fixed execute segment,
		// we can fall back to the precise simulator.
		if (UNLIKELY(!is_executable(this->pc()))) {
			this->next_execute_segment();
		}

		InstrCounter counter{machine()};

		if (imax != UINT64_MAX)
			machine().set_max_instructions(counter.value() + imax);
		else
			machine().set_max_instructions(UINT64_MAX);

restart_simulation:
		const auto* current_exec = this->m_exec;
		const auto current_begin = current_exec->exec_begin();
		const auto current_end = current_exec->exec_end();
		const auto* exec_decoder = current_exec->decoder_cache();

		auto pc = this->pc();
		do {
			// Retrieve handler directly from the instruction handler cache
			auto* decoder = &exec_decoder[pc / DecoderCache<W>::DIVISOR];
			// The number of instructions to run until we can check
			// if we ran out of instructions or PC changed.
			size_t count = decoder->idxend;
			// With compressed instructions enabled, we get the instruction
			// count from the 8-bit instr_count value.
			size_t instr_count = count+1;
			if constexpr (compressed_enabled) {
				instr_count = decoder->idxend - decoder->instr_count;
			} else {
				pc += count * 4;
			}
			counter.increment_counter(instr_count);
			auto* decoder_end = &decoder[count];
			// We want to run 4 instructions at a time, except for
			// the last one, which we will "always" do next
			if constexpr (!compressed_enabled)
			{
				while (decoder + 4 < decoder_end)
				{
					decoder[0].execute(*this);
					decoder[1].execute(*this);
					decoder[2].execute(*this);
					decoder[3].execute(*this);
					decoder += 4;
				}
			} else { // Conservative compressed version
				while (decoder + 4 < decoder_end)
				{
					// Here we try to avoid re-reading decoder memory
					// after executing instructions, by using locals.
					auto* decoder0 = decoder;
					const auto oplen0 = decoder0->opcode_length;
					auto* decoder1 = decoder0 + oplen0 / 2;
					const auto oplen1 = decoder1->opcode_length;

					decoder0->execute(*this);
					decoder1->execute(*this);

					pc += oplen0 + oplen1;
					decoder = decoder1 + oplen1 / 2;
				}
			}
			// Execute remainder with no PC-dependency
			constexpr int OFF = compressed_enabled ? 2 : 0;
			while (decoder+OFF < decoder_end) {
				// Execute instruction using handler and 32-bit wrapper
				decoder->execute(*this);
				// increment *local* PC
				if constexpr (compressed_enabled) {
					auto length = decoder->opcode_length;
					pc += length;
					decoder += length / 2;
				} else {
					decoder++;
				}
			}
			// Execute last instruction (with updated PC)
			registers().pc = pc;
			decoder->execute(*this);
			// The loop above ended because PC could have changed.
			// Update to real PC by reading from register memory.
			if constexpr (compressed_enabled)
				pc = registers().pc + decoder->opcode_length;
			else
				pc = registers().pc + 4;

			// If we left the execute segment, we have to
			// fall back to slower, precise simulation.
			if (UNLIKELY(!(pc >= current_begin && pc < current_end))) {
				break;
			}

		} while (!counter.overflowed());

		registers().pc = pc;

		// If we are here because the program is still running,
		// but we are outside the decoder cache, use precise simulation
		if (UNLIKELY(!current_exec->is_within(pc) && !counter.overflowed())) {
			// The slowpath only returns if the segment is executable
			// again, or the instruction limit was reached.
			this->next_execute_segment();
			goto restart_simulation;
		}

	} // CPU::simulate
#endif // RISCV_FAST_SIMULATOR

	template<int W>
	void CPU<W>::step_one()
	{
		// Read, decode & execute instructions directly
		auto instruction = this->read_next_instruction();
		this->execute(instruction);

		if constexpr (compressed_enabled)
			registers().pc += instruction.length();
		else
			registers().pc += 4;

		machine().increment_counter(1);
	}

	template<int W> __attribute__((cold))
	void CPU<W>::trigger_exception(int intr, address_t data)
	{
		switch (intr)
		{
		case INVALID_PROGRAM:
			throw MachineException(INVALID_PROGRAM,
				"Machine not initialized", data);
		case ILLEGAL_OPCODE:
			throw MachineException(ILLEGAL_OPCODE,
					"Illegal opcode executed", data);
		case ILLEGAL_OPERATION:
			throw MachineException(ILLEGAL_OPERATION,
					"Illegal operation during instruction decoding", data);
		case PROTECTION_FAULT:
			throw MachineException(PROTECTION_FAULT,
					"Protection fault", data);
		case EXECUTION_SPACE_PROTECTION_FAULT:
			throw MachineException(EXECUTION_SPACE_PROTECTION_FAULT,
					"Execution space protection fault", data);
		case MISALIGNED_INSTRUCTION:
			// NOTE: only check for this when jumping or branching
			throw MachineException(MISALIGNED_INSTRUCTION,
					"Misaligned instruction executed", data);
		case INVALID_ALIGNMENT:
			throw MachineException(INVALID_ALIGNMENT,
					"Invalid alignment for address", data);
		case UNIMPLEMENTED_INSTRUCTION:
			throw MachineException(UNIMPLEMENTED_INSTRUCTION,
					"Unimplemented instruction executed", data);
		case DEADLOCK_REACHED:
			throw MachineException(DEADLOCK_REACHED,
					"Atomics deadlock reached", data);

		default:
			throw MachineException(UNKNOWN_EXCEPTION,
					"Unknown exception", intr);
		}
	}

	template <int W> __attribute__((cold))
	std::string CPU<W>::to_string(format_t format, const instruction_t& instr) const
	{
		if constexpr (W == 4)
			return RV32I::to_string(*this, format, instr);
		else if constexpr (W == 8)
			return RV64I::to_string(*this, format, instr);
		else if constexpr (W == 16)
			return RV128I::to_string(*this, format, instr);
		return "Unknown architecture";
	}

	template <int W> __attribute__((cold))
	std::string CPU<W>::current_instruction_to_string() const
	{
		format_t instruction;
		try {
			instruction = this->read_next_instruction();
		} catch (...) {
			instruction = format_t {};
		}
		return isa_type<W>::to_string(*this, instruction, decode(instruction));
	}

	template <int W> __attribute__((cold))
	std::string Registers<W>::flp_to_string() const
	{
		char buffer[800];
		int  len = 0;
		for (int i = 0; i < 32; i++) {
			auto& src = this->getfl(i);
			const char T = (src.i32[1] == -1) ? 'S' : 'D';
			double val = (src.i32[1] == -1) ? src.f32[0] : src.f64;
			len += snprintf(buffer+len, sizeof(buffer) - len,
					"[%s\t%c%+.2f] ", RISCV::flpname(i), T, val);
			if (i % 5 == 4) {
				len += snprintf(buffer+len, sizeof(buffer)-len, "\n");
			}
		}
		return std::string(buffer, len);
	}

	template struct CPU<4>;
	template struct Registers<4>;
	template struct CPU<8>;
	template struct Registers<8>;
	template struct CPU<16>;
	template struct Registers<16>;
}
