#include "rv32i.hpp"
#include "machine.hpp"
#include "rv32i_instr.cpp"

namespace riscv
{
	template<>
	const CPU<4>::instruction_t& CPU<4>::decode(const format_t instruction) const
	{
		// if all bits are zero, it's an illegal instruction (by design)
		if (instruction.whole == 0x0) {
			return DECODED_INSTR(ILLEGAL);
		}

		switch (instruction.opcode())
		{
			case 0b0000011:
				return DECODED_INSTR(LOAD);
			case 0b0100011:
				return DECODED_INSTR(STORE);
			case 0b1000011:
				return DECODED_INSTR(MADD);
			case 0b1100011:
				return DECODED_INSTR(BRANCH);
			case 0b1100111:
				return DECODED_INSTR(JALR);
			case 0b1101111:
				return DECODED_INSTR(JAL);
			case 0b0010011:
				return DECODED_INSTR(OP_IMM);
			case 0b0110011:
				return DECODED_INSTR(OP);
			case 0b1110011:
				return DECODED_INSTR(SYSTEM);
			case 0b0110111:
				return DECODED_INSTR(LUI);
			case 0b0010111:
				return DECODED_INSTR(AUIPC);
			case 0b0011011:
				return DECODED_INSTR(OP_IMM32);
			case 0b0111011:
				return DECODED_INSTR(OP32);
		}
		// illegal operation exception
		return DECODED_INSTR(UNIMPLEMENTED);
	}

	std::string RV32I::to_string(CPU<4>& cpu, format_t format, const instruction_t& instr)
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
