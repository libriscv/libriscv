/**
 * Popular instructions
*/

// See: bytecode_bodies.hpp
#define BYTECODE_BODY(bc) \
	bytecode_body<W>(BcTag<bc>{}, CPU(), DECODER(), pc)

#ifdef RISCV_EXT_COMPRESSED
INSTRUCTION(RV32C_BC_ADDI, rv32c_addi) {
	BYTECODE_BODY(RV32C_BC_ADDI);
	NEXT_C_INSTR();
}
INSTRUCTION(RV32C_BC_MV, rv32c_mv) {
	BYTECODE_BODY(RV32C_BC_MV);
	NEXT_C_INSTR();
}
INSTRUCTION(RV32C_BC_SLLI, rv32c_slli) {
	BYTECODE_BODY(RV32C_BC_SLLI);
	NEXT_C_INSTR();
}
#endif
INSTRUCTION(RV32I_BC_ADDI, rv32i_addi) {
	BYTECODE_BODY(RV32I_BC_ADDI);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_LI, rv32i_li) {
	BYTECODE_BODY(RV32I_BC_LI);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_MV, rv32i_mv) {
	BYTECODE_BODY(RV32I_BC_MV);
	NEXT_INSTR();
}
#ifdef RISCV_64I
INSTRUCTION(RV64I_BC_ADDIW, rv64i_addiw) {
	BYTECODE_BODY(RV64I_BC_ADDIW);
	NEXT_INSTR();
}
#endif // RISCV_64I

INSTRUCTION(RV32I_BC_FAST_JAL, rv32i_fast_jal) {
	if constexpr (VERBOSE_JUMPS) {
		VIEW_INSTR();
		fprintf(stderr, "FAST_JAL PC 0x%lX => 0x%lX\n", long(pc), long(pc + int32_t(instr.whole)));
	}
	NEXT_BLOCK(int32_t(DECODER().instr), true);
}
INSTRUCTION(RV32I_BC_FAST_CALL, rv32i_fast_call) {
	if constexpr (VERBOSE_JUMPS) {
		VIEW_INSTR();
		fprintf(stderr, "FAST_CALL PC 0x%lX => 0x%lX\n", long(pc), long(pc + int32_t(instr.whole)));
	}
	REG(REG_RA) = pc + 4;
	NEXT_BLOCK(int32_t(DECODER().instr), true);
}

INSTRUCTION(RV32I_BC_SLLI, rv32i_slli) {
	BYTECODE_BODY(RV32I_BC_SLLI);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_SLTI, rv32i_slti) {
	BYTECODE_BODY(RV32I_BC_SLTI);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_SLTIU, rv32i_sltiu) {
	BYTECODE_BODY(RV32I_BC_SLTIU);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_XORI, rv32i_xori) {
	BYTECODE_BODY(RV32I_BC_XORI);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_SRLI, rv32i_srli) {
	BYTECODE_BODY(RV32I_BC_SRLI);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_SRAI, rv32i_srai) {
	BYTECODE_BODY(RV32I_BC_SRAI);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_ORI, rv32i_ori) {
	BYTECODE_BODY(RV32I_BC_ORI);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_ANDI, rv32i_andi) {
	BYTECODE_BODY(RV32I_BC_ANDI);
	NEXT_INSTR();
}

#ifdef RISCV_EXT_COMPRESSED
INSTRUCTION(RV32C_BC_BNEZ, rv32c_bnez) {
	VIEW_INSTR_AS(fi, FasterItype);
	if (REG(fi.get_rs1()) != 0) {
		PERFORM_BRANCH();
	}
	NEXT_BLOCK(2, false);
}
INSTRUCTION(RV32C_BC_BEQZ, rv32c_beqz) {
	VIEW_INSTR_AS(fi, FasterItype);
	if (REG(fi.get_rs1()) == 0) {
		PERFORM_BRANCH();
	}
	NEXT_BLOCK(2, false);
}
INSTRUCTION(RV32C_BC_JMP, rv32c_jmp) {
	VIEW_INSTR_AS(fi, FasterItype);
	PERFORM_BRANCH();
}
INSTRUCTION(RV32C_BC_JAL_ADDIW, rv32c_jal_addiw) {
	if constexpr (W >= 8) { // C.ADDIW
		VIEW_INSTR_AS(fi, FasterItype);
		REG(fi.get_rs1()) = (int32_t)
			((uint32_t)REG(fi.get_rs1()) + fi.signed_imm());
		NEXT_C_INSTR();
	} else { // C.JAL
		VIEW_INSTR_AS(fi, FasterItype);
		REG(REG_RA) = pc + 2;
		PERFORM_BRANCH();
	}
}
INSTRUCTION(RV32C_BC_JR, rv32c_jr) {
	VIEW_INSTR();
	if constexpr (VERBOSE_JUMPS) {
		fprintf(stderr, "C.JR from 0x%lX to 0x%lX\n",
			long(pc), long(REG(instr.whole)));
	}
	pc = REG(instr.whole) & ~addr_t(1);
	OVERFLOW_CHECKED_JUMP();
}
INSTRUCTION(RV32C_BC_JALR, rv32c_jalr) {
	VIEW_INSTR();
	const auto target = REG(instr.whole);
	if constexpr (VERBOSE_JUMPS) {
		fprintf(stderr, "C.JALR from 0x%lX to 0x%lX\n",
			long(pc), long(REG(instr.whole)));
	}
	REG(REG_RA) = pc + 2;
	pc = target & ~addr_t(1);
	OVERFLOW_CHECKED_JUMP();
}
#endif // RISCV_EXT_COMPRESSED

INSTRUCTION(RV32I_BC_JAL, rv32i_jal)
{
	VIEW_INSTR_AS(fi, FasterJtype);
	if constexpr (VERBOSE_JUMPS) {
		printf("JAL PC 0x%lX => 0x%lX\n", (long)pc, (long)pc + fi.signed_imm());
	}
	REG(fi.rd) = pc + 4;
	NEXT_BLOCK(fi.signed_imm(), true);
}

INSTRUCTION(RV32I_BC_BEQ, rv32i_beq) {
	VIEW_INSTR_AS(fi, FasterItype);
	if (REG(fi.get_rs1()) == REG(fi.get_rs2())) {
		PERFORM_BRANCH();
	}
	NEXT_BLOCK(4, false);
}
INSTRUCTION(RV32I_BC_BNE, rv32i_bne) {
	VIEW_INSTR_AS(fi, FasterItype);
	if (REG(fi.get_rs1()) != REG(fi.get_rs2())) {
		PERFORM_BRANCH();
	}
	NEXT_BLOCK(4, false);
}
INSTRUCTION(RV32I_BC_BEQ_FW, rv32i_beq_fw) {
	VIEW_INSTR_AS(fi, FasterItype);
	if (REG(fi.get_rs1()) == REG(fi.get_rs2())) {
		PERFORM_FORWARD_BRANCH();
	}
	NEXT_BLOCK(4, false);
}
INSTRUCTION(RV32I_BC_BNE_FW, rv32i_bne_fw) {
	VIEW_INSTR_AS(fi, FasterItype);
	if (REG(fi.get_rs1()) != REG(fi.get_rs2())) {
		PERFORM_FORWARD_BRANCH();
	}
	NEXT_BLOCK(4, false);
}
INSTRUCTION(RV32I_BC_BLT, rv32i_blt) {
	VIEW_INSTR_AS(fi, FasterItype);
	if ((saddr_t)REG(fi.get_rs1()) < (saddr_t)REG(fi.get_rs2())) {
		PERFORM_BRANCH();
	}
	NEXT_BLOCK(4, false);
}
INSTRUCTION(RV32I_BC_BGE, rv32i_bge) {
	VIEW_INSTR_AS(fi, FasterItype);
	if ((saddr_t)REG(fi.get_rs1()) >= (saddr_t)REG(fi.get_rs2())) {
		PERFORM_BRANCH();
	}
	NEXT_BLOCK(4, false);
}
INSTRUCTION(RV32I_BC_BLTU, rv32i_bltu) {
	VIEW_INSTR_AS(fi, FasterItype);
	if (REG(fi.get_rs1()) < REG(fi.get_rs2())) {
		PERFORM_BRANCH();
	}
	NEXT_BLOCK(4, false);
}
INSTRUCTION(RV32I_BC_BGEU, rv32i_bgeu) {
	VIEW_INSTR_AS(fi, FasterItype);
	if (REG(fi.get_rs1()) >= REG(fi.get_rs2())) {
		PERFORM_BRANCH();
	}
	NEXT_BLOCK(4, false);
}


INSTRUCTION(RV32I_BC_LDW, rv32i_ldw) {
	BYTECODE_BODY(RV32I_BC_LDW);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_STW, rv32i_stw) {
	BYTECODE_BODY(RV32I_BC_STW);
	NEXT_INSTR();
}
#ifdef RISCV_64I
INSTRUCTION(RV32I_BC_LDD, rv32i_ldd) {
	BYTECODE_BODY(RV32I_BC_LDD);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_STD, rv32i_std) {
	BYTECODE_BODY(RV32I_BC_STD);
	NEXT_INSTR();
}
#endif // RISCV_64I

INSTRUCTION(RV32F_BC_FLW, rv32i_flw) {
	BYTECODE_BODY(RV32F_BC_FLW);
	NEXT_INSTR();
}
INSTRUCTION(RV32F_BC_FLD, rv32i_fld) {
	BYTECODE_BODY(RV32F_BC_FLD);
	NEXT_INSTR();
}

#ifdef RISCV_EXT_COMPRESSED
INSTRUCTION(RV32C_BC_LDD, rv32c_ldd) {
	BYTECODE_BODY(RV32C_BC_LDD);
	NEXT_C_INSTR();
}
INSTRUCTION(RV32C_BC_STD, rv32c_std) {
	BYTECODE_BODY(RV32C_BC_STD);
	NEXT_C_INSTR();
}
INSTRUCTION(RV32C_BC_LDW, rv32c_ldw) {
	BYTECODE_BODY(RV32C_BC_LDW);
	NEXT_C_INSTR();
}
INSTRUCTION(RV32C_BC_STW, rv32c_stw) {
	BYTECODE_BODY(RV32C_BC_STW);
	NEXT_C_INSTR();
}
INSTRUCTION(RV32C_BC_FLD, rv32c_fld) {
	BYTECODE_BODY(RV32C_BC_FLD);
	NEXT_C_INSTR();
}
INSTRUCTION(RV32C_BC_FSD, rv32c_fsd) {
	BYTECODE_BODY(RV32C_BC_FSD);
	NEXT_C_INSTR();
}
INSTRUCTION(RV32C_BC_FLW, rv32c_flw) {
	BYTECODE_BODY(RV32C_BC_FLW);
	NEXT_C_INSTR();
}
INSTRUCTION(RV32C_BC_FSW, rv32c_fsw) {
	BYTECODE_BODY(RV32C_BC_FSW);
	NEXT_C_INSTR();
}
#endif // RISCV_EXT_COMPRESSED


INSTRUCTION(RV32I_BC_AUIPC, rv32i_auipc)
{
	BYTECODE_BODY(RV32I_BC_AUIPC);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_LUI, rv32i_lui)
{
	BYTECODE_BODY(RV32I_BC_LUI);
	NEXT_INSTR();
}
#ifdef RISCV_EXT_COMPRESSED
INSTRUCTION(RV32C_BC_LUI, rv32c_lui)
{
	BYTECODE_BODY(RV32C_BC_LUI);
	NEXT_C_INSTR();
}
#endif

INSTRUCTION(RV32I_BC_OP_ADD, rv32i_op_add) {
	BYTECODE_BODY(RV32I_BC_OP_ADD);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_SUB, rv32i_op_sub) {
	BYTECODE_BODY(RV32I_BC_OP_SUB);
	NEXT_INSTR();
}

INSTRUCTION(RV32I_BC_OP_SLL, rv32i_op_sll) {
	BYTECODE_BODY(RV32I_BC_OP_SLL);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_SLT, rv32i_op_slt) {
	BYTECODE_BODY(RV32I_BC_OP_SLT);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_SLTU, rv32i_op_sltu) {
	BYTECODE_BODY(RV32I_BC_OP_SLTU);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_XOR, rv32i_op_xor) {
	BYTECODE_BODY(RV32I_BC_OP_XOR);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_SRL, rv32i_op_srl) {
	BYTECODE_BODY(RV32I_BC_OP_SRL);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_OR, rv32i_op_or) {
	BYTECODE_BODY(RV32I_BC_OP_OR);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_AND, rv32i_op_and) {
	BYTECODE_BODY(RV32I_BC_OP_AND);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_MUL, rv32i_op_mul) {
	BYTECODE_BODY(RV32I_BC_OP_MUL);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_SH1ADD, rv32i_op_sh1add) {
	BYTECODE_BODY(RV32I_BC_OP_SH1ADD);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_SH2ADD, rv32i_op_sh2add) {
	BYTECODE_BODY(RV32I_BC_OP_SH2ADD);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_SH3ADD, rv32i_op_sh3add) {
	BYTECODE_BODY(RV32I_BC_OP_SH3ADD);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_SRA, rv32i_op_sra) {
	BYTECODE_BODY(RV32I_BC_OP_SRA);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_ZEXT_H, rv32i_op_zext_h) {
	BYTECODE_BODY(RV32I_BC_OP_ZEXT_H);
	NEXT_INSTR();
}

#ifdef RISCV_64I
INSTRUCTION(RV64I_BC_OP_ADDW, rv64i_op_addw) {
	BYTECODE_BODY(RV64I_BC_OP_ADDW);
	NEXT_INSTR();
}
INSTRUCTION(RV64I_BC_OP_SUBW, rv64i_op_subw) {
	BYTECODE_BODY(RV64I_BC_OP_SUBW);
	NEXT_INSTR();
}
INSTRUCTION(RV64I_BC_OP_MULW, rv64i_op_mulw) {
	BYTECODE_BODY(RV64I_BC_OP_MULW);
	NEXT_INSTR();
}
INSTRUCTION(RV64I_BC_OP_ADD_UW, rv64i_op_add_uw) {
	BYTECODE_BODY(RV64I_BC_OP_ADD_UW);
	NEXT_INSTR();
}
INSTRUCTION(RV64I_BC_SLLIW, rv64i_slliw) {
	BYTECODE_BODY(RV64I_BC_SLLIW);
	NEXT_INSTR();
}
INSTRUCTION(RV64I_BC_SRLIW, rv64i_srliw) {
	BYTECODE_BODY(RV64I_BC_SRLIW);
	NEXT_INSTR();
}
#endif // RISCV_64I

INSTRUCTION(RV32I_BC_SEXT_B, rv32i_sext_b) {
	BYTECODE_BODY(RV32I_BC_SEXT_B);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_SEXT_H, rv32i_sext_h) {
	BYTECODE_BODY(RV32I_BC_SEXT_H);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_BSETI, rv32i_bseti) {
	BYTECODE_BODY(RV32I_BC_BSETI);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_BEXTI, rv32i_bexti) {
	BYTECODE_BODY(RV32I_BC_BEXTI);
	NEXT_INSTR();
}

INSTRUCTION(RV32F_BC_FSW, rv32i_fsw) {
	BYTECODE_BODY(RV32F_BC_FSW);
	NEXT_INSTR();
}
INSTRUCTION(RV32F_BC_FSD, rv32i_fsd) {
	BYTECODE_BODY(RV32F_BC_FSD);
	NEXT_INSTR();
}

INSTRUCTION(RV32F_BC_FADD, rv32f_fadd) {
	BYTECODE_BODY(RV32F_BC_FADD);
	NEXT_INSTR();
}
INSTRUCTION(RV32F_BC_FSUB, rv32f_fsub) {
	BYTECODE_BODY(RV32F_BC_FSUB);
	NEXT_INSTR();
}
INSTRUCTION(RV32F_BC_FMUL, rv32f_fmul) {
	BYTECODE_BODY(RV32F_BC_FMUL);
	NEXT_INSTR();
}
INSTRUCTION(RV32F_BC_FDIV, rv32f_fdiv) {
	BYTECODE_BODY(RV32F_BC_FDIV);
	NEXT_INSTR();
}
INSTRUCTION(RV32F_BC_FMADD, rv32f_fmadd) {
	BYTECODE_BODY(RV32F_BC_FMADD);
	NEXT_INSTR();
}


INSTRUCTION(RV32I_BC_JALR, rv32i_jalr) {
	VIEW_INSTR_AS(fi, FasterItype);
	// jump to register + immediate
	// NOTE: if rs1 == rd, avoid clobber by storing address first
	const auto address = REG(fi.rs2) + fi.signed_imm();
	// Link *next* instruction (rd = PC + 4)
	if (fi.rs1 != 0) {
		REG(fi.rs1) = pc + 4;
	}
	if constexpr (VERBOSE_JUMPS) {
		fprintf(stderr, "JALR x%d + %d => rd=%d   PC 0x%lX => 0x%lX\n",
			fi.rs2, fi.signed_imm(), fi.rs1, long(pc), long(address));
	}
	static constexpr addr_t ALIGN_MASK = (compressed_enabled) ? 0x1 : 0x3;
	const auto target = address & ~addr_t(1);
	if constexpr (!compressed_enabled) {
		if (UNLIKELY(target & ALIGN_MASK))
			CPU().trigger_exception(MISALIGNED_INSTRUCTION, target);
	}
	pc = target;
	OVERFLOW_CHECKED_JUMP();
}

#ifdef RISCV_64I
INSTRUCTION(RV64I_BC_SRAIW, rv64i_sraiw) {
	BYTECODE_BODY(RV64I_BC_SRAIW);
	NEXT_INSTR();
}
INSTRUCTION(RV64I_BC_OP_SH1ADD_UW, rv64i_op_sh1add_uw) {
	BYTECODE_BODY(RV64I_BC_OP_SH1ADD_UW);
	NEXT_INSTR();
}
INSTRUCTION(RV64I_BC_OP_SH2ADD_UW, rv64i_op_sh2add_uw) {
	BYTECODE_BODY(RV64I_BC_OP_SH2ADD_UW);
	NEXT_INSTR();
}
#endif // RISCV_64I

INSTRUCTION(RV32I_BC_OP_DIV, rv32i_op_div) {
	BYTECODE_BODY(RV32I_BC_OP_DIV);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_DIVU, rv32i_op_divu) {
	BYTECODE_BODY(RV32I_BC_OP_DIVU);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_REM, rv32i_op_rem) {
	BYTECODE_BODY(RV32I_BC_OP_REM);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_REMU, rv32i_op_remu) {
	BYTECODE_BODY(RV32I_BC_OP_REMU);
	NEXT_INSTR();
}

INSTRUCTION(RV32I_BC_LDB, rv32i_ldb) {
	BYTECODE_BODY(RV32I_BC_LDB);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_LDBU, rv32i_ldbu) {
	BYTECODE_BODY(RV32I_BC_LDBU);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_LDH, rv32i_ldh) {
	BYTECODE_BODY(RV32I_BC_LDH);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_LDHU, rv32i_ldhu) {
	BYTECODE_BODY(RV32I_BC_LDHU);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_STB, rv32i_stb) {
	BYTECODE_BODY(RV32I_BC_STB);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_STH, rv32i_sth) {
	BYTECODE_BODY(RV32I_BC_STH);
	NEXT_INSTR();
}
#ifdef RISCV_64I
INSTRUCTION(RV32I_BC_LDWU, rv32i_ldwu) {
	BYTECODE_BODY(RV32I_BC_LDWU);
	NEXT_INSTR();
}
#endif // RISCV_64I

#ifdef RISCV_EXT_COMPRESSED
INSTRUCTION(RV32C_BC_SRLI, rv32c_srli) {
	BYTECODE_BODY(RV32C_BC_SRLI);
	NEXT_C_INSTR();
}
INSTRUCTION(RV32C_BC_ADD, rv32c_add) {
	BYTECODE_BODY(RV32C_BC_ADD);
	NEXT_C_INSTR();
}
INSTRUCTION(RV32C_BC_XOR, rv32c_xor) {
	BYTECODE_BODY(RV32C_BC_XOR);
	NEXT_C_INSTR();
}
INSTRUCTION(RV32C_BC_OR, rv32c_or) {
	BYTECODE_BODY(RV32C_BC_OR);
	NEXT_C_INSTR();
}
INSTRUCTION(RV32C_BC_SRAI, rv32c_srai) {
	BYTECODE_BODY(RV32C_BC_SRAI);
	NEXT_C_INSTR();
}
INSTRUCTION(RV32C_BC_SUB, rv32c_sub) {
	BYTECODE_BODY(RV32C_BC_SUB);
	NEXT_C_INSTR();
}
INSTRUCTION(RV32C_BC_AND, rv32c_and) {
	BYTECODE_BODY(RV32C_BC_AND);
	NEXT_C_INSTR();
}
INSTRUCTION(RV32C_BC_SUBW, rv32c_subw) {
	BYTECODE_BODY(RV32C_BC_SUBW);
	NEXT_C_INSTR();
}
INSTRUCTION(RV32C_BC_ADDW, rv32c_addw) {
	BYTECODE_BODY(RV32C_BC_ADDW);
	NEXT_C_INSTR();
}
INSTRUCTION(RV32C_BC_ANDI, rv32c_andi) {
	BYTECODE_BODY(RV32C_BC_ANDI);
	NEXT_C_INSTR();
}
INSTRUCTION(RV32C_BC_FUNCTION, rv32c_func) {
	CPU().execute(DECODER().m_handler, DECODER().instr);
	NEXT_C_INSTR();
}
#endif

#ifdef RISCV_EXT_VECTOR
// All vector instructions execute through RV32I_BC_FUNCTION.
#endif // RISCV_EXT_VECTOR

INSTRUCTION(RV32I_BC_LIVEPATCH, execute_livepatch) {
#if defined(RISCV_BINARY_TRANSLATION) || defined(RISCV_ASMJIT)
	// Native code was activated in the background. Only the bytecode of this
	// entry was replaced, so the rest of it still describes the original block.
	// 1. Wind back PC to the current decoder position
	pc = pc - DECODER().block_bytes();
#  ifdef DISPATCH_MODE_TAILCALL
	// 2. Find the correct decoder pointer in the patched decoder cache
	auto* patched = &exec->patched_decoder_cache()[pc / DecoderData<W>::DIVISOR];
	d = patched;
#  else
	// 2. Find the correct decoder pointer in the patched decoder cache
	exec_decoder = exec->patched_decoder_cache();
	decoder = &exec_decoder[pc / DecoderData<W>::DIVISOR];
#  endif
#else
	// Invalid handler
	DECODER().set_bytecode(RV32I_BC_INVALID);
	DECODER().set_invalid_handler();
#endif
	// 3. Execute the instruction
	EXECUTE_CURRENT();
}


INSTRUCTION(RV32I_BC_FUNCTION, execute_decoded_function)
{
	//printf("Slowpath: 0x%X  (instr: 0x%X)\n", uint32_t(pc), DECODER().instr);
	CPU().execute(DECODER().m_handler, DECODER().instr);
	if (UNLIKELY(exec->is_stale())) {
		pc = RECONSTRUCT_PC() + 4;
		ENTER_NEW_EXECUTE_SEGMENT();
	}
	NEXT_INSTR();
}

INSTRUCTION(RV32I_BC_FUNCBLOCK, execute_function_block) {
	VIEW_INSTR();
	REGISTERS().pc = RECONSTRUCT_PC(); // See pcrel.cpp
	CPU().execute(DECODER().m_handler, DECODER().instr);
	if (UNLIKELY(exec->is_stale())) {
		pc = RECONSTRUCT_PC() + instr.length();
		ENTER_NEW_EXECUTE_SEGMENT();
	}
	NEXT_BLOCK(instr.length(), true);
}

/**
 * Fused pairs (fused_pairs.inc): Execute two instructions by
 * directly jumping to the second instruction using NEXT_INSTR_TO,
 * which avoids an indirect jump, "guaranteeing" prediction
**/
#define FUSED_PAIR(name, a, b, A, B, G) RISCV_FUSED_IF_ ## G( \
INSTRUCTION(RV32I_BC_F_ ## name, rv_fused_ ## name) { \
	BYTECODE_BODY(A); \
	if constexpr (A >= RV32C_BC_FIRST && A <= RV32C_BC_LAST) { \
		NEXT_C_INSTR_TO(B); \
	} else { \
		NEXT_INSTR_TO(B); \
	} \
})
#include "fused_pairs.inc"
#undef FUSED_PAIR
