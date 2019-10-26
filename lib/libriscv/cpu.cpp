#include "machine.hpp"
#include "common.hpp"
#include "riscvbase.hpp"

namespace riscv
{
	template <int W>
	void CPU<W>::reset()
	{
		m_data = {};
		m_data.m_regs.pc = machine().memory.start_address();
		this->reg(RISCV::REG_SP) = machine().memory.stack_address();
		this->reg(RISCV::REG_GP) = machine().memory.stack_address();
	}

	template<int W>
	void CPU<W>::simulate()
	{
		this->break_checks();
		this->execute();
	}

	template<int W>
	void CPU<W>::execute()
	{
		const auto instruction = this->read_instruction(this->pc());
		const auto& handler = this->decode(instruction);

		// instruction logging
		if (machine().verbose_instructions)
		{
			const auto string = isa_t::to_string(*this, instruction, handler);
			printf("%s\n", string.c_str());
		}

		// execute instruction
		handler.handler(*this, instruction);
		assert(this->reg(0) == 0);

		if (machine().verbose_registers)
		{
			auto regs = this->registers().to_string();
			printf("\n%s\n\n", regs.c_str());
		}

		// allow exceptions to happen on this instruction
		this->handle_interrupts();

		// increment PC
		registers().pc += instruction.length();
	}

	template <int W>
	typename CPU<W>::format_t CPU<W>::read_instruction(address_t address)
	{
		// decode whole instruction at address
		format_t instruction;
		instruction.whole = this->machine().memory.template read<address_t>(address);

		if (instruction.length() == 4) {
			// re-read 32-bit instruction *sigh*
			instruction.whole = this->machine().memory.template read<address_t>(address);
		}

		return instruction;
	}

	template<int W>
	void CPU<W>::jump(const address_t dst)
	{
		this->registers().pc = dst;
		// it's possible to jump to a misaligned instruction
		if (this->registers().pc & 0x1) {
			this->trigger_interrupt(MISALIGNED_INSTRUCTION);
		}
	}

	template<int W>
	void CPU<W>::trigger_interrupt(interrupt_t intr)
	{
		m_data.interrupt_queue.push_back(intr);
	}

	template<int W>
	void CPU<W>::handle_interrupts()
	{
		if (UNLIKELY(m_data.interrupt_master_enable && !m_data.interrupt_queue.empty()))
		{
			for (auto intr : m_data.interrupt_queue) {
				this->execute_interrupt(intr);
			}
			m_data.interrupt_queue.clear();
		}
	}

	template<int W>
	void CPU<W>::execute_interrupt(interrupt_t intr)
	{
		// TODO: replace with callback system
		switch (intr)
		{
		case DEBUG_INTERRUPT:
			machine().system_call(0);
			break;
		case ILLEGAL_OPCODE:
			throw std::runtime_error("Illegal opcode executed");
		case ILLEGAL_OPERATION:
			throw std::runtime_error("Illegal operation during instruction decoding");
		case PROTECTION_FAULT:
			throw std::runtime_error("Protection fault");
		case MISALIGNED_INSTRUCTION:
			// NOTE: only check for this when jumping or branching
			throw std::runtime_error("Misaligned instruction executed");
		case UNIMPLEMENTED_INSTRUCTION:
			throw std::runtime_error("Unimplemented instruction executed");
		default:
			throw std::runtime_error("Interrupt not implemented: " + std::to_string(intr));
		}
	}

	template class CPU<4>;
	//template class CPU<8>;
}
