#include "rvfd.hpp"

#define FLOAT_INSTR(x, ...) \
		static CPU<4>::instruction_t instr32f_##x { __VA_ARGS__ }
#define DECODED_FLOAT(x) instr32f_##x

namespace riscv
{
	FLOAT_INSTR(FLW_FLD,
	[] (auto& cpu, rv32i_instruction instr)
	{
		rv32f_instruction fi { instr };
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		rv32f_instruction fi { instr };
		static const std::array<const char*, 8> insn {
			"???", "FLH", "FLW", "FLD", "FLQ", "???", "???", "???"
		};
		return snprintf(buffer, len, "%s %s, %s",
						insn[fi.Stype.funct3],
                        RISCV::flpname(fi.Stype.rs1),
                        RISCV::flpname(fi.Stype.rs2));
	});
	FLOAT_INSTR(FSW_FSD,
	[] (auto& cpu, rv32i_instruction instr)
	{
		rv32f_instruction fi { instr };
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		rv32f_instruction fi { instr };
		static const std::array<const char*, 8> insn {
			"???", "FSH", "FSW", "FSD", "FSQ", "???", "???", "???"
		};
		return snprintf(buffer, len, "%s %s, %s",
						insn[fi.Stype.funct3],
                        RISCV::flpname(fi.Stype.rs1),
                        RISCV::flpname(fi.Stype.rs2));
	});
}
