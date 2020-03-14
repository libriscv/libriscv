#include "rv32i.hpp"
#include "machine.hpp"
#include "rv32i_instr.cpp"
#include "rv32a_instr.cpp"
#include "rv32c_instr.cpp"
#include "rv32f_instr.cpp"

namespace riscv
{
	template<>
	const CPU<4>::instruction_t& CPU<4>::decode(const format_t instruction) const
	{
#define DECODER(x) return(x)
#include "rv32_instr.inc"
#undef DECODER
	}

	template<>
	void CPU<4>::execute(const format_t instruction)
	{
#define DECODER(x) x.handler(*this, instruction); return;
#include "rv32_instr.inc"
#undef DECODER
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
					"[%08X]     %04hX %.*s",
					cpu.pc(), (uint16_t) format.whole, ibuflen, ibuffer);
		}
		else {
			throw MachineException(UNIMPLEMENTED_INSTRUCTION_LENGTH,
									"Unimplemented instruction format length");
		}
		return std::string(buffer, len);
	}
}
