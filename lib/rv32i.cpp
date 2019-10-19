#include "rv32i.hpp"
#include "cpu.hpp"
#include "rv32i_instr.cpp"
#include <tuple>

namespace riscv
{
	template<>
	CPU<4>::CPU(Machine<4>& machine)
		: m_machine { machine }
	{

	}

	template<> inline
	CPU<4>::decoded_t CPU<4>::decode(const address_t address)
	{
		// decode whole instruction at address
		format_t instruction;

		switch (instruction.opcode())
		{
			case 0x00:
			case 0x01:
			case 0x02:
			case 0x03:
				return DECODED_INSTR(LD, instruction);
		}
		// illegal operation exception
		throw std::runtime_error("Unimplemented instruction");
	}

	template<>
	void CPU<4>::execute()
	{
		auto [instr, format] = decode(this->pc());

		// instruction logging
		char buffer[512];
		int  buflen = instr.printer(buffer, sizeof(buffer), *this, format);
		printf("[%08X] %.*s\n", this->pc(), buflen, buffer);

		// increment PC
		m_data.m_pc += 1;

		// execute instruction
		instr.handler(*this, format);
	}
}
