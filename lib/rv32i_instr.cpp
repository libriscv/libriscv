#include "rv32i.hpp"

#define INSTRUCTION(x, handler, printer) \
		static CPU<4>::instruction_t instr32i_##x { handler, printer }
#define DECODED_INSTR(x, instr) { instr32i_##x, instr }

namespace riscv
{
	INSTRUCTION(ILLEGAL,
	[] (auto& cpu, rv32i_instruction instr) {
		// illegal opcode exception
		cpu.trigger_interrupt(ILLEGAL_OPCODE);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		if (instr.opcode() == 0)
			return snprintf(buffer, len, "ILLEGAL OPCODE (Zero, outside executable area?)");
		else
			return snprintf(buffer, len, "ILLEGAL (Unknown)");
	});

	INSTRUCTION(UNIMPLEMENTED,
	[] (auto& cpu, rv32i_instruction instr) {
		// handler
		cpu.trigger_interrupt(UNIMPLEMENTED_INSTRUCTION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		return snprintf(buffer, len, "UNIMPLEMENTED: %#x (%#x)", instr.opcode(), instr.whole);
	});

	INSTRUCTION(LOAD,
	[] (auto& cpu, rv32i_instruction instr) {
		// handler
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		return snprintf(buffer, len, "LOAD");
	});

	INSTRUCTION(STORE,
	[] (auto& cpu, rv32i_instruction instr) {
		// handler
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		return snprintf(buffer, len, "STORE");
	});

	INSTRUCTION(MADD,
	[] (auto& cpu, rv32i_instruction instr) {
		// handler
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		return snprintf(buffer, len, "MADD");
	});

	INSTRUCTION(BRANCH,
	[] (auto& cpu, rv32i_instruction instr) {
		// handler
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		return snprintf(buffer, len, "BRANCH");
	});

	INSTRUCTION(JALR,
	[] (auto& cpu, rv32i_instruction instr) {
		// return back to where we came from
		// NOTE: returning from _start should exit the machine
		printf("RET called: Returning\n");
		
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		return snprintf(buffer, len, "RET");
	});

	INSTRUCTION(JAL,
	[] (auto& cpu, rv32i_instruction instr) {
		// handler
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		return snprintf(buffer, len, "JAL");
	});

	INSTRUCTION(OP_IMM,
	[] (auto& cpu, rv32i_instruction instr) {
		// handler
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		return snprintf(buffer, len, "OP_IMM");
	});

	INSTRUCTION(OP,
	[] (auto& cpu, rv32i_instruction instr) {
		// handler
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		return snprintf(buffer, len, "OP");
	});

	INSTRUCTION(SYSTEM,
	[] (auto& cpu, rv32i_instruction instr) {
		// handler
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		return snprintf(buffer, len, "SYSTEM");
	});

	INSTRUCTION(LUI,
	[] (auto& cpu, rv32i_instruction instr) {
		// handler
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		return snprintf(buffer, len, "LUI");
	});

	INSTRUCTION(OP_IMM32,
	[] (auto& cpu, rv32i_instruction instr) {
		// handler
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		return snprintf(buffer, len, "OP_IMM32");
	});

	INSTRUCTION(OP32,
	[] (auto& cpu, rv32i_instruction instr) {
		// handler
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		return snprintf(buffer, len, "OP_32");
	});
}
