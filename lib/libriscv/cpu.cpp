#include "machine.hpp"
#include "decoder_cache.hpp"
#include "instruction_counter.hpp"
#include "riscvbase.hpp"
#include "rv32i_instr.hpp"

namespace riscv
{
	// A default empty execute segment used to enforce that the
	// current CPU execute segment is never null.
	template <int W>
	static std::shared_ptr<DecodedExecuteSegment<W>> empty_shared = std::make_shared<DecodedExecuteSegment<W>>(0, 0, 0, 0);
	template <int W>
	std::shared_ptr<DecodedExecuteSegment<W>>& CPU<W>::empty_execute_segment() noexcept {
		return empty_shared<W>;
	}

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
		: m_machine { machine }, m_exec(other.cpu.m_exec), m_cpuid { cpu_id }
	{
		// Copy all registers except vectors
		// Users can still copy vector registers by assigning to registers().rvv().
		this->registers().copy_from(Registers<W>::Options::NoVectors, other.cpu.registers());
	}
	template <int W>
	void CPU<W>::reset()
	{
		this->m_regs = {};
		this->reset_stack_pointer();
		// We can't jump if there's been no ELF loader
		if (!current_execute_segment().empty()) {
			const auto initial_pc = machine().memory.start_address();
			// Check if the initial PC is executable, unless
			// the execute segment is marked as execute-only.
			if (!current_execute_segment().is_execute_only())
			{
				const auto& page =
					machine().memory.get_exec_pageno(initial_pc / riscv::Page::size());
				if (UNLIKELY(!page.attr.exec))
					trigger_exception(EXECUTION_SPACE_PROTECTION_FAULT, initial_pc);
			}
			// This function will (at most) validate the execute segment
			this->jump(initial_pc);
		}
		// reset the page cache
		this->m_cache = {};
	}

	template <int W>
	DecodedExecuteSegment<W>& CPU<W>::init_execute_area(const void* vdata, address_t begin, address_t vlength)
	{
		if (vlength < 4)
			trigger_exception(EXECUTION_SPACE_PROTECTION_FAULT, begin);

		this->m_exec = &machine().memory.create_execute_segment(
			{}, vdata, begin, vlength);
		return *this->m_exec;
	} // CPU::init_execute_area

	template<int W> RISCV_NOINLINE
	typename CPU<W>::NextExecuteReturn CPU<W>::next_execute_segment(address_t pc)
	{
		static constexpr int MAX_RESTARTS = 4;
		int restarts = 0;
restart_next_execute_segment:

		// Immediately look at the page in order to
		// verify execute and see if it has a trap handler
		auto base_pageno = pc / Page::size();
		auto end_pageno  = base_pageno + 1;
		// We absolutely need to write PC here because even read-fault handlers
		// like get_pageno() slowpaths could be reading PC.
		this->registers().pc = pc;

		// Check for +exec
		const auto& current_page =
			machine().memory.get_pageno(base_pageno);
		if (UNLIKELY(!current_page.attr.exec)) {
			this->m_fault(*this, current_page);
			pc = this->pc();

			if (UNLIKELY(++restarts == MAX_RESTARTS))
				trigger_exception(EXECUTION_LOOP_DETECTED, pc);

			goto restart_next_execute_segment;
		}

		// Check for trap
		if (UNLIKELY(current_page.has_trap()))
		{
			// We pass PC as offset
			current_page.trap(pc & (Page::size() - 1), TRAP_EXEC, pc);
			pc = this->pc();

			// If PC changed, we will restart the process
			if (pc / Page::size() != base_pageno)
			{
				if (UNLIKELY(++restarts == MAX_RESTARTS))
					trigger_exception(EXECUTION_LOOP_DETECTED, pc);

				goto restart_next_execute_segment;
			}
		}

		// Find previously decoded execute segment
		this->m_exec = machine().memory.exec_segment_for(pc).get();
		if (LIKELY(!this->m_exec->empty())) {
			return {this->m_exec, pc};
		}

		// Find decoded execute segment via override
		// If it returns empty, we build a new execute segment
		auto& next = this->m_override_exec(*this);
		if (LIKELY(!next.empty())) {
			this->m_exec = &next;
			return {this->m_exec, this->registers().pc};
		}

		// Find the earliest execute page in new segment
		while (base_pageno > 0) {
			const auto& page =
				machine().memory.get_pageno(base_pageno-1);
			if (page.attr.exec) {
				base_pageno -= 1;
			} else break;
		}

		// Find the last execute page in segment
		while (end_pageno != 0) {
			const auto& page =
				machine().memory.get_pageno(end_pageno);
			if (page.attr.exec) {
				end_pageno += 1;
			} else break;
		}

		// Allocate full execute area
		if (UNLIKELY(end_pageno <= base_pageno))
			throw MachineException(INVALID_PROGRAM, "Failed to create execute segment");
		const size_t n_pages = end_pageno - base_pageno;
		std::unique_ptr<uint8_t[]> area (new uint8_t[n_pages * Page::size()]);
		// Copy from each individual page
		for (address_t p = base_pageno; p < end_pageno; p++) {
			// Cannot use get_exec_pageno here as we may need
			// access to read fault handler.
			auto& page = machine().memory.get_pageno(p);
			const size_t offset = (p - base_pageno) * Page::size();
			std::memcpy(area.get() + offset, page.data(), Page::size());
		}

		// Decode and store it for later
		return {&this->init_execute_area(area.get(), base_pageno * Page::size(), n_pages * Page::size()), pc};
	} // CPU::next_execute_segment

	template <int W> RISCV_NOINLINE RISCV_INTERNAL
	typename CPU<W>::format_t CPU<W>::read_next_instruction_slowpath() const
	{
		// Fallback: Read directly from page memory
		const auto pageno = this->pc() / address_t(Page::size());
		// Page cache
		auto& entry = this->m_cache;
		if (entry.pageno != pageno || entry.page == nullptr) {
			// delay setting entry until we know it's good!
			auto e = decltype(m_cache){pageno, &machine().memory.get_exec_pageno(pageno)};
			if (UNLIKELY(!e.page->attr.exec)) {
				trigger_exception(EXECUTION_SPACE_PROTECTION_FAULT, this->pc());
			}
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
		return m_exec->is_within(addr);
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

	template<int W> RISCV_HOT_PATH()
	void CPU<W>::simulate_precise()
	{
		// Decoded segments are always faster
		// So, always have at least the current segment
		if (!is_executable(this->pc())) {
			this->next_execute_segment(this->pc());
		}

		auto* exec = this->m_exec;
restart_precise_sim:
		auto* exec_decoder = exec->decoder_cache();
		auto* exec_seg_data = exec->exec_data();

		for (; machine().instruction_counter() < machine().max_instructions();
			machine().increment_counter(1)) {

			format_t instruction;
			auto pc = this->pc();

			// TODO: This can me made much faster
			if (UNLIKELY(!exec->is_within(pc))) {
				// This will produce a sequential execute segment for the unknown area
				// If it is not executable, it will throw an execute space protection fault
				auto new_values = this->next_execute_segment(pc);
				exec = new_values.exec;
				pc   = new_values.pc;
				goto restart_precise_sim;
			}

			// Instructions may be unaligned with C-extension
			// On amd64 we take the cost, because it's faster
#    if defined(RISCV_EXT_COMPRESSED) && !defined(__x86_64__)
			instruction = format_t { *(UnderAlign32*) &exec_seg_data[pc] };
#    else  // aligned/unaligned loads
			instruction = format_t { *(uint32_t*) &exec_seg_data[pc] };
#    endif // aligned/unaligned loads

			constexpr bool enable_cache =
				!binary_translation_enabled;

			if constexpr (enable_cache)
			{
				// Retrieve handler directly from the instruction handler cache
				auto& cache_entry =
					exec_decoder[pc / DecoderCache<W>::DIVISOR];
				cache_entry.execute(*this, instruction);
			}
			else // Not the slowest path, since we have the instruction already
			{
				this->execute(instruction);
			}

			// increment PC
			if constexpr (compressed_enabled)
				registers().pc += instruction.length();
			else
				registers().pc += 4;
		} // while not stopped

	} // CPU::simulate_precise

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

	template<int W>
	address_type<W> CPU<W>::preempt_internal(Registers<W>& old_regs, bool store_regs, address_t pc, uint64_t max_instr)
	{
		auto& m = machine();
		const auto prev_max = m.max_instructions();
		try {
			// execute by extending the max instruction counter (resuming)
			// WARNING: Do not change this, as resumption is required in
			// order for sandbox integrity. Repeatedly invoking preemption
			// should lead to timeouts on either preempt() *or* the caller.
			m.simulate_with(
				m.instruction_counter() + max_instr, m.instruction_counter(), pc);
		} catch (...) {
			m.set_max_instructions(prev_max);
			if (store_regs) {
				this->registers() = old_regs;
			}
			throw;
		}
		// restore registers and return value
		m.set_max_instructions(prev_max);
		const auto retval = this->reg(REG_ARG0);
		if (store_regs) {
			this->registers() = old_regs;
		}
		return retval;
	}

	template<int W> RISCV_COLD_PATH()
	void CPU<W>::trigger_exception(int intr, address_t data)
	{
		switch (intr)
		{
		case INVALID_PROGRAM:
			throw MachineException(intr,
				"Machine not initialized", data);
		case ILLEGAL_OPCODE:
			throw MachineException(intr,
					"Illegal opcode executed", data);
		case ILLEGAL_OPERATION:
			throw MachineException(intr,
					"Illegal operation during instruction decoding", data);
		case PROTECTION_FAULT:
			throw MachineException(intr,
					"Protection fault", data);
		case EXECUTION_SPACE_PROTECTION_FAULT:
			throw MachineException(intr,
					"Execution space protection fault", data);
		case EXECUTION_LOOP_DETECTED:
			throw MachineException(intr,
					"Execution loop detected", data);
		case MISALIGNED_INSTRUCTION:
			// NOTE: only check for this when jumping or branching
			throw MachineException(intr,
					"Misaligned instruction executed", data);
		case INVALID_ALIGNMENT:
			throw MachineException(intr,
					"Invalid alignment for address", data);
		case UNIMPLEMENTED_INSTRUCTION:
			throw MachineException(intr,
					"Unimplemented instruction executed", data);
		case DEADLOCK_REACHED:
			throw MachineException(intr,
					"Atomics deadlock reached", data);

		default:
			throw MachineException(UNKNOWN_EXCEPTION,
					"Unknown exception", intr);
		}
	}

	template <int W> RISCV_COLD_PATH()
	std::string CPU<W>::to_string(format_t bits) const
	{
		return to_string(bits, decode(bits));
	}

	template <int W> RISCV_COLD_PATH()
	std::string CPU<W>::current_instruction_to_string() const
	{
		format_t instruction;
		try {
			instruction = this->read_next_instruction();
		} catch (...) {
			instruction = format_t {};
		}
		return to_string(instruction, decode(instruction));
	}

	template <int W> RISCV_COLD_PATH()
	std::string Registers<W>::flp_to_string() const
	{
		char buffer[800];
		int  len = 0;
		for (int i = 0; i < 32; i++) {
			auto& src = this->getfl(i);
			const char T = (src.i32[1] == 0) ? 'S' : 'D';
			if constexpr (true) {
				double val = (src.i32[1] == 0) ? src.f32[0] : src.f64;
				len += snprintf(buffer+len, sizeof(buffer) - len,
						"[%s\t%c%+.2f] ", RISCV::flpname(i), T, val);
			} else {
				if (src.i32[1] == 0) {
					double val = src.f64;
					len += snprintf(buffer+len, sizeof(buffer) - len,
							"[%s\t%c0x%lX] ", RISCV::flpname(i), T, *(int64_t *)&val);
				} else {
					float val = src.f32[0];
					len += snprintf(buffer+len, sizeof(buffer) - len,
							"[%s\t%c0x%X] ", RISCV::flpname(i), T, *(int32_t *)&val);
				}
			}
			if (i % 5 == 4) {
				len += snprintf(buffer+len, sizeof(buffer)-len, "\n");
			}
		}
		len += snprintf(buffer+len, sizeof(buffer) - len,
				"[FFLAGS\t0x%X] ", m_fcsr.fflags);
		return std::string(buffer, len);
	}

	INSTANTIATE_32_IF_ENABLED(CPU);
	INSTANTIATE_32_IF_ENABLED(Registers);
	INSTANTIATE_64_IF_ENABLED(CPU);
	INSTANTIATE_64_IF_ENABLED(Registers);
	INSTANTIATE_128_IF_ENABLED(CPU);
	INSTANTIATE_128_IF_ENABLED(Registers);
}
