#include "machine.hpp"

#include "decoder_cache.hpp"
#include "rv32i_instr.hpp"
#include "rv_disassembly.hpp"

#define INSTRUCTION(x, ...) \
	static const CPU<8>::instruction_t instr64i_##x { __VA_ARGS__ }
#define DECODED_INSTR(x) instr64i_##x
#include "rvi_instr.cpp"
#include "rvf_instr.cpp"
#ifdef RISCV_EXT_ATOMICS
#include "rva_instr.cpp"
#endif
#ifdef RISCV_EXT_COMPRESSED
#include "rvc_instr.cpp"
#endif
#ifdef RISCV_EXT_VECTOR
#include "rvv_instr.cpp"
#endif
#include "instruction_list.hpp"

namespace riscv
{
	template <> RISCV_INTERNAL
	const CPU<8>::instruction_t& CPU<8>::decode(const format_t instruction)
	{
#define DECODER(x) return(x)
#include "instr_decoding.inc"
#undef DECODER
	}

	template <> RISCV_INTERNAL
	void CPU<8>::execute(const format_t instruction)
	{
#define DECODER(x) { x.handler(*this, instruction); return; }
#include "instr_decoding.inc"
#undef DECODER
	}

	template <> RISCV_INTERNAL
	void CPU<8>::execute(uint8_t& handler_idx, uint32_t instr)
	{
		if (handler_idx == 0 && instr != 0) {
			[[unlikely]];
			handler_idx = DecoderData<8>::handler_index_for(decode(instr).handler);
		}
		DecoderData<8>::get_handlers()[handler_idx](*this, instr);
	}

	template <>
	const Instruction<8>& CPU<8>::get_unimplemented_instruction() noexcept
	{
		return DECODED_INSTR(UNIMPLEMENTED);
	}

	template <> RISCV_COLD_PATH()
	std::string Registers<8>::to_string() const
	{
		char buffer[600];
		int  len = 0;
		for (int i = 1; i < 32; i++) {
			len += snprintf(buffer+len, sizeof(buffer) - len,
					"[%s\t%08lX] ", RISCV::regname(i), (long) this->get(i));
			if (i % 5 == 4) {
				len += snprintf(buffer+len, sizeof(buffer)-len, "\n");
			}
		}
		return std::string(buffer, len);
	}

	template <> RISCV_COLD_PATH()
	std::string CPU<8>::disassemble(instruction_format format) const
	{
		char ibuffer[256];
		const int len = decode(format).printer(ibuffer, sizeof(ibuffer), *this, format);
		return std::string(ibuffer, len < 0 ? 0 : len);
	}

	template <> RISCV_COLD_PATH()
	std::string CPU<8>::to_string(instruction_format format, const instruction_t& instr) const
	{
		char buffer[512];
		char ibuffer[256];
		const int ibuflen = instr.printer(ibuffer, sizeof(ibuffer), *this, format);
		if (format.length() != 4 && format.length() != 2) {
			throw MachineException(UNIMPLEMENTED_INSTRUCTION_LENGTH,
				"Unimplemented instruction format length", format.length());
		}
		int len = rv_frame_instruction(buffer, sizeof(buffer),
			this->pc(), format.whole, format.length(), ibuffer, ibuflen);
		if (len < 0 || size_t(len) >= sizeof(buffer))
			len = int(sizeof(buffer)) - 1;
		len += rv_annotate_registers(buffer + len, sizeof(buffer) - len, *this, ibuffer);
		return std::string(buffer, len);
	}
}
