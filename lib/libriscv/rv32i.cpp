#include "rv32i.hpp"
#include "machine.hpp"
#include "rv32i_instr.cpp"
#include <tuple>

namespace riscv
{
	template<>
	CPU<4>::CPU(Machine<4>& machine)
		: m_machine { machine }
	{
		// TODO: move into header?
	}

	template<> inline
	CPU<4>::decoded_t CPU<4>::decode(const address_t address)
	{
		// decode whole instruction at address
		format_t instruction;
		instruction.whole = this->machine().memory.read<16>(address);

		// if all bits are zero, it's an illegal instruction (by design)
		if (instruction.whole == 0x0) {
			return DECODED_INSTR(ILLEGAL, instruction);
		}

		if (instruction.length() == 4) {
			// re-read 32-bit instruction *sigh*
			instruction.whole = this->machine().memory.read<32>(address);
		}

		switch (instruction.opcode())
		{
			case 0b0000011:
				return DECODED_INSTR(LOAD, instruction);
			case 0b0100011:
				return DECODED_INSTR(STORE, instruction);
			case 0b1000011:
				return DECODED_INSTR(MADD, instruction);
			case 0b1100011:
				return DECODED_INSTR(BRANCH, instruction);
			case 0b1100111:
				return DECODED_INSTR(JALR, instruction);
			case 0b1101111:
				return DECODED_INSTR(JAL, instruction);
			case 0b0010011:
				return DECODED_INSTR(OP_IMM, instruction);
			case 0b0110011:
				return DECODED_INSTR(OP, instruction);
			case 0b1110011:
				return DECODED_INSTR(SYSTEM, instruction);
			case 0b0110111:
				return DECODED_INSTR(LUI, instruction);
			case 0b0011011:
				return DECODED_INSTR(OP_IMM32, instruction);
			case 0b0111011:
				return DECODED_INSTR(OP32, instruction);
		}
		// illegal operation exception
		return DECODED_INSTR(UNIMPLEMENTED, instruction);
	}

	template<>
	void CPU<4>::execute()
	{
		auto [instr, format] = decode(this->pc());

		// instruction logging
		if (machine().verbose_instructions)
		{
			char buffer[512];
			int  buflen = instr.printer(buffer, sizeof(buffer), *this, format);
			if (format.length() == 4) {
				printf("[%08X] %08X %.*s\n", this->pc(), format.whole, buflen, buffer);
			}
			else if (format.length() == 2) {
				printf("[%08X] %04hX %.*s\n", this->pc(), (uint16_t) format.whole, buflen, buffer);
			}
			else {
				throw std::runtime_error("Unimplemented instruction format length");
			}
		}

		// execute instruction
		instr.handler(*this, format);

		if (machine().verbose_registers)
		{
			printf("\n");
			for (int i = 0; i < 32; i++) {
				printf("[%s\t%08X] ", RISCV::regname(i), this->reg(i));
				if (i % 6 == 5) printf("\n");
			}
			printf("[%s\t%08X] ", "PC", this->pc());
			printf("\n\n");
		}

		// increment PC
		m_data.pc += format.length();

		this->handle_interrupts();
	}
}
