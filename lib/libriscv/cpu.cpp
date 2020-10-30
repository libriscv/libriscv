#include "machine.hpp"
#include "common.hpp"
#include "decoder_cache.hpp"
#include "riscvbase.hpp"
#include "rv32i_instr.hpp"
#include "rv64i.hpp"

namespace riscv
{
	template <int W>
	void CPU<W>::reset()
	{
		this->m_regs = {};
		this->reset_stack_pointer();
		// jumping causes some extra calculations
		this->jump(machine().memory.start_address());
		// reset the page cache
		this->m_cache = {};
	}

	template <int W> __attribute__((noinline))
	typename CPU<W>::format_t CPU<W>::read_next_instruction_slowpath()
	{
		// Fallback: Read directly from page memory
		const auto pageno = this->pc() >> Page::SHIFT;
		// Page cache
		auto& entry = this->m_cache;
		if (entry.pageno != pageno) {
			entry = {pageno, &machine().memory.get_exec_pageno(pageno)};
			if (!entry.page->attr.exec) {
				trigger_exception(EXECUTION_SPACE_PROTECTION_FAULT, this->pc());
			}
		}
		const auto& page = *entry.page;
		const auto offset = this->pc() & (Page::size()-1);
		format_t instruction;

		if (LIKELY(offset <= Page::size()-4)) {
			instruction.whole = *(uint32_t*) (page.data() + offset);
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
	typename CPU<W>::format_t CPU<W>::read_next_instruction()
	{
		if (LIKELY(this->pc() >= m_exec_begin && this->pc() < m_exec_end)) {
			return format_t { *(uint32_t*) &m_exec_data[this->pc()] };
		}

		return read_next_instruction_slowpath();
	}

	template<int W>
	void CPU<W>::simulate()
	{
		format_t instruction;
#ifdef RISCV_DEBUG
		this->break_checks();

		instruction = this->read_next_instruction();
		const auto& handler = this->decode(instruction);
		// instruction logging
		if (machine().verbose_instructions)
		{
			const auto string = isa_type<W>::to_string(*this, instruction, handler);
			printf("%s\n", string.c_str());
		}

		// execute instruction
		handler.handler(*this, instruction);
#else
# ifdef RISCV_INSTR_CACHE
		if (LIKELY(this->pc() >= m_exec_begin && this->pc() < m_exec_end)) {
			instruction = format_t { *(uint32_t*) &m_exec_data[this->pc()] };
			// retrieve instructions directly from the constant cache
			auto& cache_entry =
				machine().memory.get_decoder_cache()[this->pc() / DecoderCache<W>::DIVISOR];
		#ifndef RISCV_INSTR_CACHE_PREGEN
			if (UNLIKELY(!cache_entry)) {
				cache_entry = this->decode(instruction).handler;
			}
		#endif
			// execute instruction
			cache_entry(*this, instruction);
		} else {
			instruction = read_next_instruction_slowpath();
			// decode & execute instruction directly
			this->execute(instruction);
		}
# else
		instruction = this->read_next_instruction();
		// decode & execute instruction directly
		this->execute(instruction);
# endif
#endif
		// increment instruction counter
		this->m_counter++;

#ifdef RISCV_DEBUG
		if (UNLIKELY(machine().verbose_registers))
		{
			auto regs = this->registers().to_string();
			printf("\n%s\n\n", regs.c_str());
			if (UNLIKELY(machine().verbose_fp_registers)) {
				printf("%s\n", registers().flp_to_string().c_str());
			}
		}
#endif

		// increment PC
		if constexpr (compressed_enabled)
			registers().pc += instruction.length();
		else
			registers().pc += 4;
	}

	template<int W> __attribute__((cold))
	void CPU<W>::trigger_exception(interrupt_t intr, address_t data)
	{
		switch (intr)
		{
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
		case UNIMPLEMENTED_INSTRUCTION:
			throw MachineException(UNIMPLEMENTED_INSTRUCTION,
					"Unimplemented instruction executed", data);
		default:
			throw MachineException(UNKNOWN_EXCEPTION,
					"Unknown exception", intr);
		}
	}

	template <int W> __attribute__((cold))
	std::string Registers<W>::to_string() const
	{
		char buffer[600];
		int  len = 0;
		for (int i = 1; i < 32; i++) {
			len += snprintf(buffer+len, sizeof(buffer) - len,
					"[%s\t%08lX] ", RISCV::regname(i), (long) this->get(i));
			if (i % 5 == 4) {
				len += snprintf(buffer+len, sizeof(buffer)-len, "\n");
			}
		}
		return std::string(buffer, len);
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
}
