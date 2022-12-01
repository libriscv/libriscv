#include "rv32i_instr.hpp"
#include "machine.hpp"
#include "rv128i.hpp"
#undef RISCV_EXT_COMPRESSED
#undef RISCV_EXT_ATOMICS

#define INSTRUCTION(x, ...) \
	CPU<16>::instruction_t instr128i_##x { __VA_ARGS__ }
#define DECODED_INSTR(x) instr128i_##x
#include "rvi_instr.cpp"
#include "rvf_instr.cpp"
#ifdef RISCV_EXT_ATOMICS
#include "rva_instr.cpp"
#endif
#ifdef RISCV_EXT_VECTOR
#include "rvv_instr.cpp"
#endif
#include "instruction_list.hpp"

namespace riscv
{
	template <> RISCV_INTERNAL
	const CPU<16>::instruction_t &CPU<16>::decode(const format_t instruction)
	{
#define DECODER(x) return(x)
#include "instr_decoding.inc"
#undef DECODER
	}

	template <> RISCV_INTERNAL
	void CPU<16>::execute(const format_t instruction)
	{
#define DECODER(x) { x.handler(*this, instruction); return; }
#include "instr_decoding.inc"
#undef DECODER
	}

	template <> __attribute__((cold))
	std::string Registers<16>::to_string() const
	{
		char buffer[1800];
		int  len = 0;
		char regbuffer[32];

		for (int i = 1; i < 32; i++) {
			const int reglen =
				RV128I::to_hex(regbuffer, sizeof(regbuffer), this->get(i));
			len += snprintf(buffer+len, sizeof(buffer) - len,
					"[%s\t%.*s] ", RISCV::regname(i), reglen, regbuffer);
			if (i % 5 == 4) {
				len += snprintf(buffer+len, sizeof(buffer)-len, "\n");
			}
		}
		return std::string(buffer, len);
	}

	std::string RV128I::to_string(const CPU<16>& cpu, instruction_format format, const instruction_t& instr)
	{
		char buffer[512];
		char ibuffer[256];
		int  ibuflen = instr.printer(ibuffer, sizeof(ibuffer), cpu, format);
		int  len = 0;
		char pcbuffer[32];
		int pclen = RV128I::to_hex(pcbuffer, sizeof(pcbuffer), cpu.pc());
		if (format.length() == 4) {
			len = snprintf(buffer, sizeof(buffer),
					"[0x%.*s] %08X %.*s",
					pclen, pcbuffer, format.whole, ibuflen, ibuffer);
		}
		else if (format.length() == 2) {
			len = snprintf(buffer, sizeof(buffer),
					"[0x%.*s]     %04hX %.*s",
					pclen, pcbuffer, (uint16_t) format.whole, ibuflen, ibuffer);
		}
		else {
			throw MachineException(UNIMPLEMENTED_INSTRUCTION_LENGTH,
				"Unimplemented instruction format length", format.length());
		}
		return std::string(buffer, len);
	}
}
