#pragma once
#include "rv_printer.hpp"
#include <cinttypes>
#include <cstring>
#include <string>

/**
 * Glue between the objdump-exact instruction printers and CPU::to_string().
 *
 * The printers themselves emit nothing but the objdump text, so that a
 * disassembly can be diffed against binutils without any post-processing (see
 * tests/disasm/). Everything a human wants on top of that -- the address, the
 * raw encoding, the live values of the registers involved -- is added here.
 *
 * Included by rv32i.cpp / rv64i.cpp / rv128i.cpp, each of which defines the
 * CPU<W>::to_string and CPU<W>::disassemble specialisation for its width.
 */
namespace riscv
{
	/// Frame one instruction as `[0xPC] <encoding> <objdump text>`.
	inline int rv_frame_instruction(char* buffer, size_t len,
		uint64_t pc, uint32_t whole, uint32_t length, const char* text, int textlen)
	{
		if (textlen < 0) textlen = 0;
		if (length == 2)
			return snprintf(buffer, len, "[0x%" PRIX64 "]     %04" PRIx16 " %.*s",
				pc, uint16_t(whole), textlen, text);
		return snprintf(buffer, len, "[0x%" PRIX64 "] %08" PRIx32 " %.*s",
			pc, whole, textlen, text);
	}

	/// True when `token` is one of the 32 integer ABI register names.
	inline int rv_integer_regno(const char* token, size_t toklen) noexcept
	{
		for (int i = 0; i < 32; i++) {
			const char* name = RVPRINT::reg(i);
			if (strlen(name) == toklen && memcmp(name, token, toklen) == 0)
				return i;
		}
		return -1;
	}

	/// True when `token` is one of the 32 floating-point ABI register names.
	inline int rv_float_regno(const char* token, size_t toklen) noexcept
	{
		for (int i = 0; i < 32; i++) {
			const char* name = RVPRINT::freg(i);
			if (strlen(name) == toklen && memcmp(name, token, toklen) == 0)
				return i;
		}
		return -1;
	}

	/**
	 * Append ` ; a0=0x1 a1=0x2` to a disassembled instruction, listing the live
	 * value of every register it names.
	 *
	 * This works off the printed operand text rather than the instruction
	 * fields, so it needs no per-instruction knowledge and stays correct as
	 * printers are added. `zero` is skipped, as are repeats of a register that
	 * was already listed.
	 */
	template <int W>
	inline int rv_annotate_registers(char* buffer, size_t len,
		const CPU<W>& cpu, const char* text)
	{
		// Operands start after the tab that follows the mnemonic.
		const char* operands = strchr(text, '\t');
		if (operands == nullptr)
			return 0;
		operands += 1;

		uint32_t seen_int = 0, seen_flt = 0;
		size_t written = 0;
		const char* p = operands;
		while (*p != '\0' && written + 1 < len)
		{
			// Split on everything that cannot be part of a register name.
			if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))) {
				p++;
				continue;
			}
			const char* start = p;
			while ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))
				p++;
			const size_t toklen = size_t(p - start);
			const char* separator = (written == 0) ? " ; " : " ";

			// snprintf reports what it *would* have written, so clamp before
			// advancing or the next call would run off the end of the buffer.
			int n = 0;
			const int ireg = rv_integer_regno(start, toklen);
			const int freg = (ireg > 0) ? -1 : rv_float_regno(start, toklen);
			if (ireg > 0 && !(seen_int & (1u << ireg))) {
				seen_int |= 1u << ireg;
				n = snprintf(buffer + written, len - written, "%s%s=0x%" PRIX64,
					separator, RVPRINT::reg(ireg), uint64_t(cpu.reg(ireg)));
			} else if (freg >= 0 && !(seen_flt & (1u << freg))) {
				seen_flt |= 1u << freg;
				n = snprintf(buffer + written, len - written, "%s%s=%f",
					separator, RVPRINT::freg(freg), cpu.registers().getfl(freg).f64);
			}
			if (n < 0)
				break;
			written += size_t(n);
			if (written >= len) {
				written = len - 1;
				break;
			}
		}
		return int(written);
	}
}
