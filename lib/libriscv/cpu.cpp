#include "machine.hpp"
#include "common.hpp"
#include "riscvbase.hpp"
#include "rv32i.cpp"

namespace riscv
{
	template <int W>
	void CPU<W>::reset()
	{
		m_data = {};
		m_data.m_regs.pc = machine().memory.start_address();
		// initial stack location
		this->reg(RISCV::REG_SP) = machine().memory.stack_initial();
		// NOTE: if the stack is very low, some stack pointer value could
		// become 0x0 which could alter the behavior of the program,
		// even though the address might be legitimate. To solve this, we move
		// the stack at that time to a safer location.
		if (this->reg(RISCV::REG_SP) < 0x100000) {
			this->reg(RISCV::REG_SP) = 0x40000000;
		}
	}

	template<int W>
	void CPU<W>::simulate()
	{
#ifdef RISCV_DEBUG
		this->break_checks();
#endif
		this->execute();
	}

	template <int W>
	typename CPU<W>::format_t CPU<W>::read_instruction(address_t address)
	{
		format_t instruction;
#ifndef RISCV_DEBUG
		const uint32_t this_page = address >> Page::SHIFT;
		if (UNLIKELY(this_page != m_current_page)) {
			m_current_page = this_page;
			m_page_pointer = &machine().memory.get_page(address);
			// TODO: verify execute
			if (UNLIKELY(!m_page_pointer->attr.exec)) {
				this->trigger_exception(PROTECTION_FAULT);
			}
		}
		const auto offset = address & (Page::size()-1);

		if (LIKELY(offset < Page::size() - W))
		{
			// we can read the whole thing
			instruction.whole = *(address_t*) (m_page_pointer->data() + offset);
			return instruction;
		}
		// read short instruction at address
		instruction.whole = *(uint16_t*) (m_page_pointer->data() + offset);
#else
		// in debug mode we need a full memory read to allow trapping
		instruction.whole = this->machine().memory.template read<uint16_t>(address);
#endif

		if (instruction.length() == 4) {
			// complete the instruction (NOTE: might cross into another page)
			instruction.half[1] =
				this->machine().memory.template read<uint16_t>(address + 2);
		}
		return instruction;
	}

	template<int W>
	void CPU<W>::execute()
	{
		const auto instruction = this->read_instruction(this->pc());

#ifdef RISCV_DEBUG
		const auto& handler = this->decode(instruction);
		// instruction logging
		if (UNLIKELY(machine().verbose_instructions))
		{
			const auto string = isa_t::to_string(*this, instruction, handler);
			printf("%s\n", string.c_str());
		}

		// execute instruction
		handler.handler(*this, instruction);
#else
		// execute instruction
		this->execute(instruction);
#endif
		// increment instruction counter
		registers().counter++;

#ifdef RISCV_DEBUG
		if (UNLIKELY(machine().verbose_registers))
		{
			auto regs = this->registers().to_string();
			printf("\n%s\n\n", regs.c_str());
		}
		if (UNLIKELY(this->reg(0) != 0)) {
			const auto string = isa_t::to_string(*this, instruction, handler);
			printf("%s\n", string.c_str());
			assert(this->reg(0) == 0);
		}
#endif

		// increment PC
		registers().pc += instruction.length();
	}

	template<int W>
	void CPU<W>::trigger_exception(interrupt_t intr)
	{
		// TODO: replace with callback system
		switch (intr)
		{
		case DEBUG_INTERRUPT:
			machine().system_call(riscv::EBREAK_SYSCALL);
			break;
		case ILLEGAL_OPCODE:
			throw MachineException("Illegal opcode executed");
		case ILLEGAL_OPERATION:
			throw MachineException("Illegal operation during instruction decoding");
		case PROTECTION_FAULT:
			throw MachineException("Protection fault");
		case MISALIGNED_INSTRUCTION:
			// NOTE: only check for this when jumping or branching
			throw MachineException("Misaligned instruction executed");
		case UNIMPLEMENTED_INSTRUCTION:
			throw MachineException("Unimplemented instruction executed");
		default:
			throw MachineException("Interrupt not implemented: " + std::to_string(intr));
		}
	}

	template class CPU<4>;
	//template class CPU<8>;
}
