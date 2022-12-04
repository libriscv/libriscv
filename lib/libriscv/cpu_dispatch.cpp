#include "machine.hpp"
#include "decoder_cache.hpp"
#include "instruction_counter.hpp"
#include "instruction_list.hpp"
#include "threaded_bytecodes.hpp"
#include "rv32i_instr.hpp"
#include "rvfd.hpp"
#ifdef RISCV_EXT_COMPRESSED
#include "rvc.hpp"
#endif
#ifdef RISCV_EXT_VECTOR
#include "rvv.hpp"
#endif

namespace riscv
{
#define VIEW_INSTR() \
	auto instr = decoder->view_instr();
#define VIEW_INSTR_AS(name, x) \
	auto name = decoder->template view_instr<x>();

#ifdef DISPATCH_MODE_SWITCH_BASED

	/** Switch-case based dispatch **/
#define NEXT_INSTR()                  \
	if constexpr (compressed_enabled) \
		decoder += 2;                 \
	else                              \
		decoder += 1;                 \
	break;
#define NEXT_C_INSTR()                \
	decoder += 1;                     \
	break;

#else

	/** Threaded dispatch  **/
#define NEXT_INSTR()                  \
	if constexpr (compressed_enabled) \
		decoder += 2;                 \
	else                              \
		decoder += 1;                 \
	goto *computed_opcode[decoder->get_bytecode()];
#define NEXT_C_INSTR() \
	decoder += 1;      \
	goto *computed_opcode[decoder->get_bytecode()];

#endif // Dispatch mode

#define NEXT_BLOCK(len)               \
	pc += len;                        \
	if constexpr (compressed_enabled) \
		decoder += 2;                 \
	else                              \
		decoder += 1;                 \
	goto continue_block;
#define PERFORM_BRANCH()            \
	pc += instr.Btype.signed_imm(); \
	goto check_jump;
#define PERFORM_FAST_BRANCH() \
	pc += fi.signed_imm(); \
	goto check_jump;

template <int W> __attribute__((hot))
void CPU<W>::DISPATCH_FUNC(uint64_t imax)
{
	static constexpr uint32_t XLEN = W * 8;
	using addr_t  = address_type<W>;
	using saddr_t = std::make_signed_t<addr_t>;

#ifndef DISPATCH_MODE_SWITCH_BASED
	static constexpr void *computed_opcode[] = {
		[RV32I_BC_INVALID] = &&execute_invalid,
		[RV32I_BC_ADDI]    = &&rv32i_addi,
		[RV32I_BC_LI]      = &&rv32i_li,
		[RV32I_BC_SLLI]    = &&rv32i_slli,
		[RV32I_BC_SLTI]    = &&rv32i_slti,
		[RV32I_BC_SLTIU]   = &&rv32i_sltiu,
		[RV32I_BC_XORI]    = &&rv32i_xori,
		[RV32I_BC_SRLI]    = &&rv32i_srli,
		[RV32I_BC_SRAI]    = &&rv32i_srai,
		[RV32I_BC_ORI]     = &&rv32i_ori,
		[RV32I_BC_ANDI]    = &&rv32i_andi,

		[RV32I_BC_LUI]     = &&rv32i_lui,
		[RV32I_BC_AUIPC]   = &&rv32i_auipc,

		[RV32I_BC_LDB]     = &&rv32i_ldb,
		[RV32I_BC_LDBU]    = &&rv32i_ldbu,
		[RV32I_BC_LDH]     = &&rv32i_ldh,
		[RV32I_BC_LDHU]    = &&rv32i_ldhu,
		[RV32I_BC_LDW]     = &&rv32i_ldw,
		[RV32I_BC_LDWU]    = &&rv32i_ldwu,
		[RV32I_BC_LDD]     = &&rv32i_ldd,

		[RV32I_BC_STB]     = &&rv32i_stb,
		[RV32I_BC_STH]     = &&rv32i_sth,
		[RV32I_BC_STW]     = &&rv32i_stw,
		[RV32I_BC_STD]     = &&rv32i_std,

		[RV32I_BC_BEQ]     = &&rv32i_beq,
		[RV32I_BC_BNE]     = &&rv32i_bne,
		[RV32I_BC_BLT]     = &&rv32i_blt,
		[RV32I_BC_BGE]     = &&rv32i_bge,
		[RV32I_BC_BLTU]    = &&rv32i_bltu,
		[RV32I_BC_BGEU]    = &&rv32i_bgeu,

		[RV32I_BC_JAL]     = &&rv32i_jal,
		[RV32I_BC_JALR]    = &&rv32i_jalr,
		[RV32I_BC_FAST_JAL] = &&rv32i_fast_jal,

		[RV32I_BC_OP_ADD]  = &&rv32i_op_add,
		[RV32I_BC_OP_SUB]  = &&rv32i_op_sub,
		[RV32I_BC_OP_SLL]  = &&rv32i_op_sll,
		[RV32I_BC_OP_SLT]  = &&rv32i_op_slt,
		[RV32I_BC_OP_SLTU] = &&rv32i_op_sltu,
		[RV32I_BC_OP_XOR]  = &&rv32i_op_xor,
		[RV32I_BC_OP_SRL]  = &&rv32i_op_srl,
		[RV32I_BC_OP_OR]   = &&rv32i_op_or,
		[RV32I_BC_OP_AND]  = &&rv32i_op_and,
		[RV32I_BC_OP_MUL]  = &&rv32i_op_mul,
		[RV32I_BC_OP_MULH] = &&rv32i_op_mulh,
		[RV32I_BC_OP_MULHSU] = &&rv32i_op_mulhsu,
		[RV32I_BC_OP_MULHU]= &&rv32i_op_mulhu,
		[RV32I_BC_OP_DIV]  = &&rv32i_op_div,
		[RV32I_BC_OP_DIVU] = &&rv32i_op_divu,
		[RV32I_BC_OP_REM]  = &&rv32i_op_rem,
		[RV32I_BC_OP_REMU] = &&rv32i_op_remu,
		[RV32I_BC_OP_SRA]  = &&rv32i_op_sra,
		[RV32I_BC_OP_SH1ADD] = &&rv32i_op_sh1add,
		[RV32I_BC_OP_SH2ADD] = &&rv32i_op_sh2add,
		[RV32I_BC_OP_SH3ADD] = &&rv32i_op_sh3add,

#ifdef RISCV_EXT_COMPRESSED
		[RV32C_BC_ADDI]     = &&rv32c_addi,
		[RV32C_BC_LI]       = &&rv32c_addi,
		[RV32C_BC_MV]       = &&rv32c_addi,
		[RV32C_BC_FUNCTION] = &&rv32c_func,
		[RV32C_BC_JUMPFUNC] = &&rv32c_jfunc,
#endif

		[RV32I_BC_SYSCALL] = &&rv32i_syscall,
		[RV32I_BC_SYSTEM]  = &&rv32i_system,
		[RV32I_BC_NOP]     = &&rv32i_nop,

		[RV32F_BC_FLW]     = &&rv32i_flw,
		[RV32F_BC_FLD]     = &&rv32i_fld,
		[RV32F_BC_FSW]     = &&rv32i_fsw,
		[RV32F_BC_FSD]     = &&rv32i_fsd,
		[RV32F_BC_FADD]    = &&rv32f_fadd,
		[RV32F_BC_FSUB]    = &&rv32f_fsub,
		[RV32F_BC_FMUL]    = &&rv32f_fmul,
		[RV32F_BC_FDIV]    = &&rv32f_fdiv,
#ifdef RISCV_EXT_VECTOR
		[RV32V_BC_VLE32]   = &&rv32v_vle32,
		[RV32V_BC_VSE32]   = &&rv32v_vse32,
#endif
		[RV32I_BC_FUNCTION] = &&execute_decoded_function,
#ifdef RISCV_BINARY_TRANSLATION
		[RV32I_BC_TRANSLATOR] = &&translated_function,
#endif
	};
#endif

	// We need an execute segment matching current PC
	if (UNLIKELY(!is_executable(this->pc())))
	{
		this->next_execute_segment();
	}

	// Calculate the instruction limit
	if (imax != UINT64_MAX)
		machine().set_max_instructions(machine().instruction_counter() + imax);
	else
		machine().set_max_instructions(UINT64_MAX);

	InstrCounter counter{machine()};

	DecodedExecuteSegment<W>* exec = this->m_exec;
	DecoderData<W>* exec_decoder;
	DecoderData<W>* decoder;
	address_t current_begin;
	address_t current_end;
	address_t pc = this->pc();
restart_sim:
	exec_decoder = exec->decoder_cache();
	current_begin = exec->exec_begin();
	current_end = exec->exec_end();

continue_segment:
	decoder = &exec_decoder[pc / DecoderCache<W>::DIVISOR];

continue_block:
	if constexpr (compressed_enabled) {
		pc += decoder->idxend * 2;
		counter.increment_counter(decoder->instr_count);
	} else {
		unsigned count = decoder->idxend;
		pc += count * 4;
		counter.increment_counter(count + 1);
	}

#ifdef DISPATCH_MODE_SWITCH_BASED

while (true) {
	switch (decoder->get_bytecode()) {
	#define INSTRUCTION(bc, lbl) case bc

#else

	goto *computed_opcode[decoder->get_bytecode()];
	#define INSTRUCTION(bc, lbl) lbl

#endif

/** Instruction handlers **/
#ifdef RISCV_EXT_COMPRESSED
INSTRUCTION(RV32C_BC_ADDI, rv32c_addi): {
	VIEW_INSTR_AS(fi, FasterItype);
	this->reg(fi.rs1) = reg(fi.rs2) + fi.signed_imm();
	NEXT_C_INSTR();
}
#endif
INSTRUCTION(RV32I_BC_ADDI, rv32i_addi): {
	VIEW_INSTR_AS(fi, FasterItype);
	this->reg(fi.rs1) =
		this->reg(fi.rs2) + fi.signed_imm();
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_LI, rv32i_li): {
	VIEW_INSTR_AS(fi, FasterItype);
	this->reg(fi.rs1) = fi.signed_imm();
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_LDW, rv32i_ldw): {
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = this->reg(fi.rs2) + fi.signed_imm();
	this->reg(fi.rs1) =
		(int32_t)machine().memory.template read<uint32_t>(addr);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_STW, rv32i_stw): {
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr  = reg(fi.rs1) + fi.signed_imm();
	machine().memory.template write<uint32_t>(addr, reg(fi.rs2));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_LDWU, rv32i_ldwu): {
	if constexpr (W >= 8) {
		VIEW_INSTR_AS(fi, FasterItype);
		const auto addr = this->reg(fi.rs2) + fi.signed_imm();
		this->reg(fi.rs1) =
			machine().memory.template read<uint32_t>(addr);
		NEXT_INSTR();
	}
}
INSTRUCTION(RV32I_BC_LDD, rv32i_ldd): {
	if constexpr (W >= 8) {
		VIEW_INSTR_AS(fi, FasterItype);
		const auto addr = this->reg(fi.rs2) + fi.signed_imm();
		this->reg(fi.rs1) =
			(int64_t)machine().memory.template read<uint64_t>(addr);
		NEXT_INSTR();
	}
}
INSTRUCTION(RV32I_BC_STD, rv32i_std): {
	if constexpr (W >= 8) {
		VIEW_INSTR_AS(fi, FasterItype);
		const auto addr  = reg(fi.rs1) + fi.signed_imm();
		machine().memory.template write<uint64_t>(addr, reg(fi.rs2));
		NEXT_INSTR();
	}
}
INSTRUCTION(RV32I_BC_BEQ, rv32i_beq): {
	VIEW_INSTR_AS(fi, FasterItype);
	if (reg(fi.rs1) == reg(fi.rs2)) {
		PERFORM_FAST_BRANCH();
	}
	NEXT_BLOCK(4);
}
INSTRUCTION(RV32I_BC_BNE, rv32i_bne): {
	VIEW_INSTR_AS(fi, FasterItype);
	if (reg(fi.rs1) != reg(fi.rs2)) {
		PERFORM_FAST_BRANCH();
	}
	NEXT_BLOCK(4);
}
INSTRUCTION(RV32I_BC_BLT, rv32i_blt): {
	VIEW_INSTR_AS(fi, FasterItype);
	if ((saddr_t)reg(fi.rs1) < (saddr_t)reg(fi.rs2)) {
		PERFORM_FAST_BRANCH();
	}
	NEXT_BLOCK(4);
}
INSTRUCTION(RV32I_BC_BGE, rv32i_bge): {
	VIEW_INSTR_AS(fi, FasterItype);
	if ((saddr_t)reg(fi.rs1) >= (saddr_t)reg(fi.rs2)) {
		PERFORM_FAST_BRANCH();
	}
	NEXT_BLOCK(4);
}
INSTRUCTION(RV32I_BC_BLTU, rv32i_bltu): {
	VIEW_INSTR_AS(fi, FasterItype);
	if (reg(fi.rs1) < reg(fi.rs2)) {
		PERFORM_FAST_BRANCH();
	}
	NEXT_BLOCK(4);
}
INSTRUCTION(RV32I_BC_BGEU, rv32i_bgeu): {
	VIEW_INSTR_AS(fi, FasterItype);
	if (reg(fi.rs1) >= reg(fi.rs2)) {
		PERFORM_FAST_BRANCH();
	}
	NEXT_BLOCK(4);
}
INSTRUCTION(RV32I_BC_OP_ADD, rv32i_op_add): {
	#define OP_INSTR() \
		VIEW_INSTR_AS(fi, FasterOpType); \
		auto& dst = reg(fi.rd); \
		const auto src1 = reg(fi.rs1); \
		const auto src2 = reg(fi.rs2);
	OP_INSTR();
	dst = src1 + src2;
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_SUB, rv32i_op_sub): {
	OP_INSTR();
	dst = src1 - src2;
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_SLLI, rv32i_slli): {
	VIEW_INSTR_AS(fi, FasterItype);
	// SLLI: Logical left-shift 5/6/7-bit immediate
	this->reg(fi.rs1) =
		this->reg(fi.rs2) << (fi.unsigned_imm() & (XLEN - 1));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_SLTI, rv32i_slti): {
	VIEW_INSTR_AS(fi, FasterItype);
	// SLTI: Set less than immediate
	this->reg(fi.rs1) = (saddr_t(reg(fi.rs2)) < fi.signed_imm());
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_SLTIU, rv32i_sltiu): {
	VIEW_INSTR_AS(fi, FasterItype);
	// SLTIU: Sign-extend, then treat as unsigned
	this->reg(fi.rs1) = (reg(fi.rs2) < addr_t(fi.signed_imm()));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_XORI, rv32i_xori): {
	VIEW_INSTR_AS(fi, FasterItype);
	// XORI
	this->reg(fi.rs1) = reg(fi.rs2) ^ fi.signed_imm();
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_SRLI, rv32i_srli): {
	VIEW_INSTR_AS(fi, FasterItype);
	// SRLI: Shift-right logical 5/6/7-bit immediate
	this->reg(fi.rs1) = reg(fi.rs2) >> (fi.unsigned_imm() & (XLEN - 1));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_SRAI, rv32i_srai): {
	VIEW_INSTR_AS(fi, FasterItype);
	// SRAI: Shift-right arithmetical (preserve the sign bit)
	this->reg(fi.rs1) = saddr_t(reg(fi.rs2)) >> (fi.unsigned_imm() & (XLEN - 1));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_ORI, rv32i_ori): {
	VIEW_INSTR_AS(fi, FasterItype);
	// ORI: Or sign-extended 12-bit immediate
	this->reg(fi.rs1) = reg(fi.rs2) | fi.signed_imm();
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_ANDI, rv32i_andi): {
	VIEW_INSTR_AS(fi, FasterItype);
	// ANDI: And sign-extended 12-bit immediate
	this->reg(fi.rs1) = reg(fi.rs2) & fi.signed_imm();
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_JAL, rv32i_jal): {
	VIEW_INSTR_AS(fi, FasterJtype);
	if (fi.rd != 0)
		reg(fi.rd) = pc + 4;
	pc += fi.offset;
	goto check_jump;
}
INSTRUCTION(RV32I_BC_FAST_JAL, rv32i_fast_jal): {
	VIEW_INSTR();
	pc = instr.whole;
	goto check_jump;
}
INSTRUCTION(RV32I_BC_JALR, rv32i_jalr): {
	VIEW_INSTR();
	// jump to register + immediate
	// NOTE: if rs1 == rd, avoid clobber by storing address first
	const auto address = reg(instr.Itype.rs1) + instr.Itype.signed_imm();
	// Link *next* instruction (rd = PC + 4)
	if (instr.Itype.rd != 0) {
		reg(instr.Itype.rd) = pc + 4;
	}
	pc = address;
	goto check_unaligned_jump;
}
INSTRUCTION(RV32I_BC_LUI, rv32i_lui): {
	VIEW_INSTR();
	this->reg(instr.Utype.rd) = instr.Utype.upper_imm();
	NEXT_INSTR();
}

#ifdef RISCV_EXT_COMPRESSED
INSTRUCTION(RV32C_BC_FUNCTION, rv32c_func): {
	VIEW_INSTR();
	auto handler = decoder->get_handler();
	handler(*this, instr);
	NEXT_C_INSTR();
}
INSTRUCTION(RV32C_BC_JUMPFUNC, rv32c_jfunc): {
	VIEW_INSTR();
	registers().pc = pc;
	auto handler = decoder->get_handler();
	handler(*this, instr);
	pc = registers().pc + 2;
	goto check_unaligned_jump;
}
#endif

INSTRUCTION(RV32I_BC_OP_SLL, rv32i_op_sll): {
	OP_INSTR();
	dst = src1 << (src2 & (XLEN - 1));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_SLT, rv32i_op_slt): {
	OP_INSTR();
	dst = (saddr_t(src1) < saddr_t(src2));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_SLTU, rv32i_op_sltu): {
	OP_INSTR();
	dst = (src1 < src2);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_XOR, rv32i_op_xor): {
	OP_INSTR();
	dst = src1 ^ src2;
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_SRL, rv32i_op_srl): {
	OP_INSTR();
	dst = src1 >> (src2 & (XLEN - 1));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_OR, rv32i_op_or): {
	OP_INSTR();
	dst = src1 | src2;
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_AND, rv32i_op_and): {
	OP_INSTR();
	dst = src1 & src2;
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_MUL, rv32i_op_mul): {
	OP_INSTR();
	dst = saddr_t(src1) * saddr_t(src2);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_DIV, rv32i_op_div): {
	OP_INSTR();
	// division by zero is not an exception
	if (LIKELY(saddr_t(src2) != 0)) {
		if constexpr (W == 8) {
			// vi_instr.cpp:444:2: runtime error:
			// division of -9223372036854775808 by -1 cannot be represented in type 'long'
			if (LIKELY(!((int64_t)src1 == INT64_MIN && (int64_t)src2 == -1ll)))
				dst = saddr_t(src1) / saddr_t(src2);
		} else {
			// rv32i_instr.cpp:301:2: runtime error:
			// division of -2147483648 by -1 cannot be represented in type 'int'
			if (LIKELY(!(src1 == 2147483648 && src2 == 4294967295)))
				dst = saddr_t(src1) / saddr_t(src2);
		}
	} else {
		dst = addr_t(-1);
	}
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_SH1ADD, rv32i_op_sh1add): {
	OP_INSTR();
	dst = src2 + (src1 << 1);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_SH2ADD, rv32i_op_sh2add): {
	OP_INSTR();
	dst = src2 + (src1 << 2);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_SH3ADD, rv32i_op_sh3add): {
	OP_INSTR();
	dst = src2 + (src1 << 3);
	NEXT_INSTR();
}

INSTRUCTION(RV32I_BC_SYSCALL, rv32i_syscall): {
	// Make the current PC visible
	this->registers().pc = pc;
	// Make the instruction counter visible
	counter.apply();
	// Invoke system call
	machine().system_call(this->reg(REG_ECALL));
	// Restore counter
	counter.retrieve();
	if (UNLIKELY(counter.overflowed() || pc != this->registers().pc))
	{
		// System calls are always full-length instructions
		pc = registers().pc + 4;
		goto check_jump;
	}
	NEXT_BLOCK(4);
}

INSTRUCTION(RV32I_BC_LDB, rv32i_ldb): {
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = this->reg(fi.rs2) + fi.signed_imm();
	this->reg(fi.rs1) =
		int8_t(machine().memory.template read<uint8_t>(addr));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_LDBU, rv32i_ldbu): {
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = this->reg(fi.rs2) + fi.signed_imm();
	this->reg(fi.rs1) =
		saddr_t(machine().memory.template read<uint8_t>(addr));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_LDH, rv32i_ldh): {
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = this->reg(fi.rs2) + fi.signed_imm();
	this->reg(fi.rs1) =
		int16_t(machine().memory.template read<uint16_t>(addr));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_LDHU, rv32i_ldhu): {
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = this->reg(fi.rs2) + fi.signed_imm();
	this->reg(fi.rs1) =
		saddr_t(machine().memory.template read<uint16_t>(addr));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_STB, rv32i_stb): {
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = reg(fi.rs1) + fi.signed_imm();
	machine().memory.template write<uint8_t>(addr, reg(fi.rs2));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_STH, rv32i_sth): {
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = reg(fi.rs1) + fi.signed_imm();
	machine().memory.template write<uint16_t>(addr, reg(fi.rs2));
	NEXT_INSTR();
}

INSTRUCTION(RV32F_BC_FLW, rv32i_flw): {
	VIEW_INSTR_AS(fi, rv32f_instruction);
	auto addr = reg(fi.Itype.rs1) + fi.Itype.signed_imm();
	auto& dst = registers().getfl(fi.Itype.rd);
	dst.load_u32(machine().memory.template read<uint32_t> (addr));
	NEXT_INSTR();
}
INSTRUCTION(RV32F_BC_FLD, rv32i_fld): {
	VIEW_INSTR_AS(fi, rv32f_instruction);
	auto addr = reg(fi.Itype.rs1) + fi.Itype.signed_imm();
	auto& dst = registers().getfl(fi.Itype.rd);
	dst.load_u64(machine().memory.template read<uint64_t> (addr));
	NEXT_INSTR();
}
INSTRUCTION(RV32F_BC_FSW, rv32i_fsw): {
	VIEW_INSTR_AS(fi, rv32f_instruction);
	const auto& src = registers().getfl(fi.Stype.rs2);
	auto addr = reg(fi.Stype.rs1) + fi.Stype.signed_imm();
	machine().memory.template write<uint32_t> (addr, src.i32[0]);
	NEXT_INSTR();
}
INSTRUCTION(RV32F_BC_FSD, rv32i_fsd): {
	VIEW_INSTR_AS(fi, rv32f_instruction);
	const auto& src = registers().getfl(fi.Stype.rs2);
	auto addr = reg(fi.Stype.rs1) + fi.Stype.signed_imm();
	machine().memory.template write<uint64_t> (addr, src.i64);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_SYSTEM, rv32i_system): {
	VIEW_INSTR();
	machine().system(instr);
	// Check if machine stopped
	if (UNLIKELY(counter.overflowed()))
	{
		registers().pc = pc + 4;
		return;
	}
	NEXT_BLOCK(4);
}
INSTRUCTION(RV32F_BC_FADD, rv32f_fadd): {
	VIEW_INSTR_AS(fi, rv32f_instruction);
	#define FLREGS() \
		auto& dst = registers().getfl(fi.R4type.rd); \
		const auto& rs1 = registers().getfl(fi.R4type.rs1); \
		const auto& rs2 = registers().getfl(fi.R4type.rs2);
	FLREGS();
	if (fi.R4type.funct2 == 0x0)
	{ // float32
		dst.set_float(rs1.f32[0] + rs2.f32[0]);
	}
	else if (fi.R4type.funct2 == 0x1)
	{ // float64
		dst.f64 = rs1.f64 + rs2.f64;
	}
	NEXT_INSTR();
}
INSTRUCTION(RV32F_BC_FSUB, rv32f_fsub): {
	VIEW_INSTR_AS(fi, rv32f_instruction);
	FLREGS();
	if (fi.R4type.funct2 == 0x0)
	{ // float32
		dst.set_float(rs1.f32[0] - rs2.f32[0]);
	}
	else if (fi.R4type.funct2 == 0x1)
	{ // float64
		dst.f64 = rs1.f64 - rs2.f64;
	}
	NEXT_INSTR();
}
INSTRUCTION(RV32F_BC_FMUL, rv32f_fmul): {
	VIEW_INSTR_AS(fi, rv32f_instruction);
	FLREGS();
	if (fi.R4type.funct2 == 0x0)
	{ // float32
		dst.set_float(rs1.f32[0] * rs2.f32[0]);
	}
	else if (fi.R4type.funct2 == 0x1)
	{ // float64
		dst.f64 = rs1.f64 * rs2.f64;
	}
	NEXT_INSTR();
}
INSTRUCTION(RV32F_BC_FDIV, rv32f_fdiv): {
	VIEW_INSTR_AS(fi, rv32f_instruction);
	FLREGS();
	if (fi.R4type.funct2 == 0x0)
	{ // float32
		dst.set_float(rs1.f32[0] / rs2.f32[0]);
	}
	else if (fi.R4type.funct2 == 0x1)
	{ // float64
		dst.f64 = rs1.f64 / rs2.f64;
	}
	NEXT_INSTR();
}

/** UNLIKELY INSTRUCTIONS **/
/** UNLIKELY INSTRUCTIONS **/

INSTRUCTION(RV32I_BC_FUNCTION, execute_decoded_function): {
	VIEW_INSTR();
	auto handler = decoder->get_handler();
	handler(*this, instr);
	NEXT_INSTR();
}

INSTRUCTION(RV32I_BC_NOP, rv32i_nop): {
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_AUIPC, rv32i_auipc): {
	VIEW_INSTR();
	this->reg(instr.Utype.rd) = pc + instr.Utype.upper_imm();
	NEXT_BLOCK(4);
}
INSTRUCTION(RV32I_BC_OP_SRA, rv32i_op_sra): {
	OP_INSTR();
	dst = saddr_t(src1) >> (src2 & (XLEN-1));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_MULH, rv32i_op_mulh): {
	OP_INSTR();
	if constexpr (W == 4) {
		dst = uint64_t((int64_t)saddr_t(src1) * (int64_t)saddr_t(src2)) >> 32u;
	} else if constexpr (W == 8) {
		dst = ((__int128_t) src1 * (__int128_t) src2) >> 64u;
	} else {
		dst = 0;
	}
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_MULHSU, rv32i_op_mulhsu): {
	OP_INSTR();
	if constexpr (W == 4) {
		dst = uint64_t((int64_t)saddr_t(src1) * (uint64_t)src2) >> 32u;
	} else if constexpr (W == 8) {
		dst = ((__int128_t) src1 * (__int128_t) src2) >> 64u;
	} else {
		dst = 0;
	}
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_MULHU, rv32i_op_mulhu): {
	OP_INSTR();
	if constexpr (W == 4) {
		dst = uint64_t((uint64_t)src1 * (uint64_t)src2) >> 32u;
	} else if constexpr (W == 8) {
		dst = ((__int128_t) src1 * (__int128_t) src2) >> 64u;
	} else {
		dst = 0;
	}
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_DIVU, rv32i_op_divu): {
	OP_INSTR();
	if (LIKELY(src2 != 0)) {
		dst = src1 / src2;
	} else {
		dst = addr_t(-1);
	}
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_REM, rv32i_op_rem): {
	OP_INSTR();
	if (LIKELY(src2 != 0)) {
		if constexpr(W == 4) {
			if (LIKELY(!(src1 == 2147483648 && src2 == 4294967295)))
				dst = saddr_t(src1) % saddr_t(src2);
		} else if constexpr (W == 8) {
			if (LIKELY(!((int64_t)src1 == INT64_MIN && (int64_t)src2 == -1ll)))
				dst = saddr_t(src1) % saddr_t(src2);
		} else {
			dst = saddr_t(src1) % saddr_t(src2);
		}
	}
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_REMU, rv32i_op_remu): {
	OP_INSTR();
	if (LIKELY(src2 != 0)) {
		dst = src1 % src2;
	} else {
		dst = addr_t(-1);
	}
	NEXT_INSTR();
}
#ifdef RISCV_EXT_VECTOR
INSTRUCTION(RV32V_BC_VLE32, rv32v_vle32): {
	VIEW_INSTR_AS(vi, rv32v_instruction);
	const auto addr = reg(vi.VLS.rs1) & ~address_t(VectorLane::size()-1);
	registers().rvv().get(vi.VLS.vd) =
		machine().memory.template read<VectorLane> (addr);
	NEXT_INSTR();
}
INSTRUCTION(RV32V_BC_VSE32, rv32v_vse32): {
	VIEW_INSTR_AS(vi, rv32v_instruction);
	const auto addr = reg(vi.VLS.rs1) & ~address_t(VectorLane::size()-1);
	auto& dst = registers().rvv().get(vi.VLS.vd);
	machine().memory.template write<VectorLane> (addr, dst);
	NEXT_INSTR();
}
#endif // RISCV_EXT_VECTOR

/** UNLIKELY INSTRUCTIONS **/
/** UNLIKELY INSTRUCTIONS **/

#ifdef RISCV_BINARY_TRANSLATION
INSTRUCTION(RV32I_BC_TRANSLATOR, translated_function): {
	VIEW_INSTR();
	counter.apply();
	auto handler = decoder->get_handler();
	handler(*this, instr);
	// Restore instruction counter and PC
	counter.retrieve();
	pc = registers().pc;
	goto check_jump;
}
#endif

#ifdef DISPATCH_MODE_SWITCH_BASED
	default:
		this->trigger_exception(ILLEGAL_OPCODE);
		__builtin_unreachable();
	} // switch case
} // while loop

#else
	execute_invalid:
		this->trigger_exception(ILLEGAL_OPCODE);
		__builtin_unreachable();
#endif

check_jump: {
	if (UNLIKELY(counter.overflowed())) {
		registers().pc = pc;
		return;
	}
	if (UNLIKELY(!(pc >= current_begin && pc < current_end)))
	{
		// We have to store and restore PC here as there are
		// custom callbacks when changing segments that can
		// jump around.
		registers().pc = pc;
		// Change execute segment
		exec = this->next_execute_segment();
		pc = registers().pc;
		// Restart with new execute boundaries
		goto restart_sim;
	}
	goto continue_segment;
}
check_unaligned_jump:
	if constexpr (!compressed_enabled) {
		if (UNLIKELY(pc & 0x3)) {
			registers().pc = pc;
			trigger_exception(MISALIGNED_INSTRUCTION, this->pc());
		}
	} else {
		if (UNLIKELY(pc & 0x1)) {
			registers().pc = pc;
			trigger_exception(MISALIGNED_INSTRUCTION, this->pc());
		}
	}
	goto check_jump;

} // CPU::simulate_XXX()

} // riscv
