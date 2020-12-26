#include "rv32i_instr.hpp"
#include "machine.hpp"

#define INSTRUCTION(x, ...) static constexpr CPU<8>::instruction_t instr64i_##x { __VA_ARGS__ }
#define DECODED_INSTR(x) instr64i_##x
#include "rvi_instr.cpp"
#ifdef RISCV_EXT_ATOMICS
#include "rva_instr.cpp"
#endif
#include "rvc_instr.cpp"
#include "rvf_instr.cpp"
#include "instruction_list.hpp"

namespace riscv
{
	template<>
	const CPU<8>::instruction_t& CPU<8>::decode(const format_t instruction) const
	{
#define DECODER(x) return(x)
#include "instr_decoding.inc"
#undef DECODER
	}

	template<>
	void CPU<8>::execute(const format_t instruction)
	{
#define DECODER(x) { x.handler(*this, instruction); return; }
#include "instr_decoding.inc"
#undef DECODER
	}

	std::string RV64I::to_string(CPU<8>& cpu, instruction_format<8> format, const instruction_t& instr)
	{
		char buffer[256];
		char ibuffer[128];
		int  ibuflen = instr.printer(ibuffer, sizeof(ibuffer), cpu, format);
		int  len = 0;
		if (format.length() == 4) {
			len = snprintf(buffer, sizeof(buffer),
					"[0x%lX] %08X %.*s",
					cpu.pc(), format.whole, ibuflen, ibuffer);
		}
		else if (format.length() == 2) {
			len = snprintf(buffer, sizeof(buffer),
					"[0x%lX]     %04hX %.*s",
					cpu.pc(), (uint16_t) format.whole, ibuflen, ibuffer);
		}
		else {
			throw MachineException(UNIMPLEMENTED_INSTRUCTION_LENGTH,
				"Unimplemented instruction format length", format.length());
		}
		return std::string(buffer, len);
	}

#ifdef RISCV_BINARY_TRANSLATION
#include "tr_emit.cpp"
template void CPU<8>::emit(std::string&, const std::string&, address_t, instr_pair*, size_t) const;
#endif

#include "rvi_fuse.cpp"
template bool CPU<8>::try_fuse(instr_pair, instr_pair) const;
}
