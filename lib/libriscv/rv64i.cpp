#include "rv32i_instr.hpp"
#include "machine.hpp"

#define INSTRUCTION(x, ...) static constexpr CPU<8>::instruction_t instr64i_##x { __VA_ARGS__ }
#define DECODED_INSTR(x) instr64i_##x
#include "rv32i_instr.cpp"
#ifdef RISCV_EXT_ATOMICS
#include "rv32a_instr.cpp"
#endif
#include "rv32c_instr.cpp"
#include "rv32f_instr.cpp"

namespace riscv
{
	template<>
	const CPU<8>::instruction_t& CPU<8>::decode(const format_t instruction) const
	{
#define DECODER(x) return(x)
#include "rv32_instr.inc"
#undef DECODER
	}

	template<>
	void CPU<8>::execute(const format_t instruction)
	{
#define DECODER(x) x.handler(*this, instruction); return;
#include "rv32_instr.inc"
#undef DECODER
	}

	std::string RV64I::to_string(CPU<8>& cpu, format_t format, const instruction_t& instr)
	{
		char buffer[256];
		char ibuffer[128];
		int  ibuflen = instr.printer(ibuffer, sizeof(ibuffer), cpu, format);
		int  len = 0;
		if (format.length() == 4) {
			len = snprintf(buffer, sizeof(buffer),
					"[%08lX] %08lX %.*s",
					cpu.pc(), format.whole, ibuflen, ibuffer);
		}
		else if (format.length() == 2) {
			len = snprintf(buffer, sizeof(buffer),
					"[%08lX]     %04hX %.*s",
					cpu.pc(), (uint16_t) format.whole, ibuflen, ibuffer);
		}
		else {
			throw MachineException(UNIMPLEMENTED_INSTRUCTION_LENGTH,
				"Unimplemented instruction format length", format.length());
		}
		return std::string(buffer, len);
	}
}
