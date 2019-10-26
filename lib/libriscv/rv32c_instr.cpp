#include "rv32c.hpp"

#define COMPRESSED_INSTR(x, ...) INSTRUCTION(x, __VA_ARGS__)
#define DECODED_COMPR(x) DECODED_INSTR(x)
#define CI_CODE(x, y) ((x << 13) | (y))

namespace riscv
{
	// LW, LD, LQ, FLW, FLD, SW, SD, SQ, FSW, FSD
	COMPRESSED_INSTR(C0_ADDI4SPN,
	[] (auto& cpu, rv32i_instruction instr) {
		// TODO: implement me
		cpu.trigger_interrupt(UNIMPLEMENTED_INSTRUCTION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		auto ci = instr.compressed();
		return snprintf(buffer, len, "C.ADDI4SPN");
	});
	COMPRESSED_INSTR(C0_REG_LOAD,
	[] (auto& cpu, rv32i_instruction instr) {
		// TODO: implement me
		cpu.trigger_interrupt(UNIMPLEMENTED_INSTRUCTION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		static std::array<const char*, 4> f3 = {
			"???", "FLD", "LW", "FLW"
		};
		auto ci = instr.compressed();
		return snprintf(buffer, len, "C.%s %s",
						f3[ci.CL.funct3], RISCV::ciname(ci.CL.srs1));
	});
	COMPRESSED_INSTR(C0_REG_STORE,
	[] (auto& cpu, rv32i_instruction instr) {
		// TODO: implement me
		cpu.trigger_interrupt(UNIMPLEMENTED_INSTRUCTION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		static std::array<const char*, 4> f3 = {
			"Illegal instruction", "FSD", "SW", "FSW"
		};
		auto ci = instr.compressed();
		return snprintf(buffer, len, "C.%s %s",
						f3[ci.CS.funct3], RISCV::ciname(ci.CS.srs1));
	});

	COMPRESSED_INSTR(C1_NOP_ADDI,
	[] (auto& cpu, rv32i_instruction instr) {
		auto ci = instr.compressed();
		// ADDI
		if (ci.CI.rd != 0)
		{
			cpu.reg(ci.CI.rd) += ci.CI.signed_imm();
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		auto ci = instr.compressed();
		if (ci.CI.imm1 != 0 && ci.CI.rd != 0 && ci.CI.imm2 != 0) {
			return snprintf(buffer, len, "C.ADDI %s, %d",
							RISCV::regname(ci.CI.rd), ci.CI.signed_imm());
		}
		if (ci.CI.imm1 != 0 || ci.CI.imm2 != 0)
			return snprintf(buffer, len, "C.HINT");
		return snprintf(buffer, len, "C.NOP");
	});

	COMPRESSED_INSTR(C1_JAL_ADDIW,
	[] (auto& cpu, rv32i_instruction instr) {
		// TODO: implement me
		cpu.trigger_interrupt(UNIMPLEMENTED_INSTRUCTION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		auto ci = instr.compressed();
		return snprintf(buffer, len, "C.JAL");
	});

	COMPRESSED_INSTR(C1_LI,
	[] (auto& cpu, rv32i_instruction instr) {
		// TODO: implement me
		cpu.trigger_interrupt(UNIMPLEMENTED_INSTRUCTION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		auto ci = instr.compressed();
		return snprintf(buffer, len, "C.LI");
	});

	COMPRESSED_INSTR(C1_ADDI16SP_LUI,
	[] (auto& cpu, rv32i_instruction instr) {
		// TODO: implement me
		cpu.trigger_interrupt(UNIMPLEMENTED_INSTRUCTION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		auto ci = instr.compressed();
		return snprintf(buffer, len, "C.ADDI16SP / C.LUI");
	});

	COMPRESSED_INSTR(C1_ALU_OPS,
	[] (auto& cpu, rv32i_instruction instr) {
		// TODO: implement me
		cpu.trigger_interrupt(UNIMPLEMENTED_INSTRUCTION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		auto ci = instr.compressed();
		return snprintf(buffer, len, "C.ALU_OPS");
	});

	COMPRESSED_INSTR(C1_JUMP,
	[] (auto& cpu, rv32i_instruction instr) {
		// TODO: implement me
		cpu.trigger_interrupt(UNIMPLEMENTED_INSTRUCTION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		auto ci = instr.compressed();
		return snprintf(buffer, len, "C.JMP");
	});

	COMPRESSED_INSTR(C1_BEQZ,
	[] (auto& cpu, rv32i_instruction instr) {
		// TODO: implement me
		cpu.trigger_interrupt(UNIMPLEMENTED_INSTRUCTION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		auto ci = instr.compressed();
		return snprintf(buffer, len, "C.BEQZ");
	});

	COMPRESSED_INSTR(C1_BNEZ,
	[] (auto& cpu, rv32i_instruction instr) {
		// TODO: implement me
		cpu.trigger_interrupt(UNIMPLEMENTED_INSTRUCTION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		auto ci = instr.compressed();
		return snprintf(buffer, len, "C.BNEZ");
	});

	// LWSP, LDSP, LQSP, FLWSP, FLDSP, SWSP, SDSP, SQSP, FSWSP, FSDSP
	COMPRESSED_INSTR(C2_SP_LOAD,
	[] (auto& cpu, rv32i_instruction instr) {
		// TODO: implement me
		cpu.trigger_interrupt(UNIMPLEMENTED_INSTRUCTION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		static std::array<const char*, 4> f3 = {
			"SLLI", "FLDSP", "LWSP", "FLWSP"
		};
		auto ci = instr.compressed();
		return snprintf(buffer, len, "C.%s %s",
						f3[ci.CI.funct3], RISCV::ciname(ci.CI.rd));
	});
	COMPRESSED_INSTR(C2_SP_STORE,
	[] (auto& cpu, rv32i_instruction instr) {
		// TODO: implement me
		cpu.trigger_interrupt(UNIMPLEMENTED_INSTRUCTION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		static std::array<const char*, 4> f3 = {
			"XXX", "FSDSP", "SWSP", "FSWSP"
		};
		auto ci = instr.compressed();
		return snprintf(buffer, len, "C.%s %s",
						f3[ci.CSS.funct3 >> 1], RISCV::ciname(ci.CSS.rs2));
	});
	// JR, MV, EBREAK, JALR, ADD
	COMPRESSED_INSTR(C2_VARIOUS,
	[] (auto& cpu, rv32i_instruction instr) {
		auto ci = instr.compressed();
		// TODO: implement me
		cpu.trigger_interrupt(UNIMPLEMENTED_INSTRUCTION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int
	{
		auto ci = instr.compressed();
		return snprintf(buffer, len, "C.JR");
	});

}
