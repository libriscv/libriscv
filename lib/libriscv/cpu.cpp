#include "machine.hpp"
#include "common.hpp"
#include "riscvbase.hpp"

namespace riscv
{
	template <int W>
	void CPU<W>::reset()
	{
		m_regs = {};
		// initial stack location
		this->reg(RISCV::REG_SP) = machine().memory.stack_initial();
		// NOTE: if the stack is very low, some stack pointer value could
		// become 0x0 which could alter the behavior of the program,
		// even though the address might be legitimate. To solve this, we move
		// the stack at that time to a safer location.
		if (this->reg(RISCV::REG_SP) < 0x100000) {
			this->reg(RISCV::REG_SP) = 0x40000000;
		}
		// jumping causes some extra calculations
		this->jump(machine().memory.start_address());
	}

	template <int W>
	typename CPU<W>::format_t CPU<W>::read_instruction(address_t address)
	{
		format_t instruction;
#ifndef RISCV_DEBUG
		const address_t this_page = address & ~(Page::size()-1);
		if (this_page != this->m_current_page) {
			this->change_page(this_page);
		}
		const address_t offset = address & (Page::size()-1);

#ifndef RISCV_EXT_COMPRESSED
		// special case for non-compressed mode:
		// we can always read whole instructions
		instruction.whole = *(uint32_t*) (m_page_pointer->data() + offset);
		return instruction;
#else
		// here we support compressed instructions
		// read only full-sized instructions until the end of the buffer
		if (LIKELY(offset <= Page::size() - 4))
		{
			// we can read the whole thing
			instruction.whole = *(uint32_t*) (m_page_pointer->data() + offset);
			return instruction;
		}

		// read short instruction at address
		instruction.whole = *(uint16_t*) (m_page_pointer->data() + offset);

		// read upper half, completing a 32-bit instruction
		if (instruction.is_long()) {
			// this instruction crosses a page-border
			this->change_page(this->m_current_page + Page::size());
			instruction.half[1] = *(uint16_t*) m_page_pointer->data();
		}
#endif
#else
		// in debug mode we need a full memory read to allow trapping
		if ((address & (W-1)) == 0) {
			instruction.whole = this->machine().memory.template read<address_t>(address);
		}
		else
		{
			// instruction is not on word-border, so do up to two smaller reads
			instruction.whole = this->machine().memory.template read<uint16_t>(address);
			if (UNLIKELY(instruction.is_long())) {
				// complete the instruction (NOTE: might cross into another page)
				instruction.half[1] =
					this->machine().memory.template read<uint16_t>(address + 2);
			}
		}
#endif
		return instruction;
	}

	template<int W>
	void CPU<W>::simulate()
	{
#ifdef RISCV_DEBUG
		this->break_checks();
#endif
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
			if (UNLIKELY(machine().verbose_fp_registers)) {
				printf("%s\n", registers().flp_to_string().c_str());
			}
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
		case ILLEGAL_OPCODE:
			throw MachineException(ILLEGAL_OPCODE,
									"Illegal opcode executed");
		case ILLEGAL_OPERATION:
			throw MachineException(ILLEGAL_OPERATION,
									"Illegal operation during instruction decoding");
		case PROTECTION_FAULT:
			throw MachineException(PROTECTION_FAULT,
									"Protection fault");
		case EXECUTION_SPACE_PROTECTION_FAULT:
			throw MachineException(EXECUTION_SPACE_PROTECTION_FAULT,
									"Execution space protection fault");
		case MISALIGNED_INSTRUCTION:
			// NOTE: only check for this when jumping or branching
			throw MachineException(MISALIGNED_INSTRUCTION,
									"Misaligned instruction executed");
		case UNIMPLEMENTED_INSTRUCTION:
			throw MachineException(UNIMPLEMENTED_INSTRUCTION,
									"Unimplemented instruction executed");
		default:
			throw MachineException(UNKNOWN_EXCEPTION, "Unknown exception", intr);
		}
	}

	template struct CPU<4>;
	//template struct CPU<8>;
}
