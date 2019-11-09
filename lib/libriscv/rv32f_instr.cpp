#include "rvf.hpp"

#define FLOAT_INSTR(x, ...) \
		static CPU<4>::instruction_t instr32f_##x { __VA_ARGS__ }
#define DECODED_FLOAT(x) instr32f_##x

namespace riscv
{
	FLOAT_INSTR(FLW,
	[] (auto& cpu, rv32i_instruction instr)
	{
		rv32f_instruction fi { instr };
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		rv32f_instruction fi { instr };
		return snprintf(buffer, len, "FLW %s, %s",
                        RISCV::regname(instr.Atype.rs1),
                        RISCV::regname(instr.Atype.rd));
	});
}
