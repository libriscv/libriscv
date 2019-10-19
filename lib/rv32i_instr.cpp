#include "rv32i.hpp"

#define INSTRUCTION(x, handler, printer) \
		static CPU<4>::instruction_t instr32i_##x { handler, printer }
#define DECODED_INSTR(x, instr) { instr32i_##x, instr }

namespace riscv
{
	INSTRUCTION(LD,
	[] (auto& cpu, rv32i_instruction instr) {
		// handler
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		return snprintf(buffer, len, "LD");
	});
}
