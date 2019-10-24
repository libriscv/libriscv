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
			case 0b0010111:
				return DECODED_INSTR(AUIPC, instruction);
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
			const auto string = isa_t::to_string(*this, format, instr);
			printf("%s\n", string.c_str());
		}

		// execute instruction
		instr.handler(*this, format);
		assert(this->reg(0) == 0);

		if (machine().verbose_registers)
		{
			auto regs = this->registers().to_string();
			printf("\n%s\n\n", regs.c_str());
		}

		// allow exceptions to happen on this instruction
		this->handle_interrupts();

		// increment PC
		registers().pc += format.length();
	}

	std::string RV32I::to_string(CPU<4>& cpu, format_t format, instruction_t instr)
	{
		char buffer[256];
		char ibuffer[128];
		int  ibuflen = instr.printer(ibuffer, sizeof(ibuffer), cpu, format);
		int  len = 0;
		if (format.length() == 4) {
			len = snprintf(buffer, sizeof(buffer),
					"[%08X] %08X %.*s",
					cpu.pc(), format.whole, ibuflen, ibuffer);
		}
		else if (format.length() == 2) {
			len = snprintf(buffer, sizeof(buffer),
					"[%08X] %04hX %.*s",
					cpu.pc(), (uint16_t) format.whole, ibuflen, ibuffer);
		}
		else {
			throw std::runtime_error("Unimplemented instruction format length");
		}
		return std::string(buffer, len);
	}
}
