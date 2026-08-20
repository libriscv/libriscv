/**
 * Popular instructions
*/

#ifdef RISCV_EXT_COMPRESSED
INSTRUCTION(RV32C_BC_ADDI, rv32c_addi) {
	VIEW_INSTR_AS(fi, FasterItype);
	REG(fi.get_rs1()) = REG(fi.get_rs2()) + fi.signed_imm();
	NEXT_C_INSTR();
}
INSTRUCTION(RV32C_BC_MV, rv32c_mv) {
	VIEW_INSTR_AS(fi, FasterMove);
	REG(fi.get_rd()) = REG(fi.get_rs1());
	NEXT_C_INSTR();
}
INSTRUCTION(RV32C_BC_SLLI, rv32c_slli) {
	VIEW_INSTR_AS(fi, FasterItype);
	REG(fi.get_rs1()) <<= fi.imm;
	NEXT_C_INSTR();
}
#endif
INSTRUCTION(RV32I_BC_ADDI, rv32i_addi) {
	VIEW_INSTR_AS(fi, FasterItype);
	REG(fi.get_rs1()) =
		REG(fi.get_rs2()) + fi.signed_imm();
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_LI, rv32i_li) {
	VIEW_INSTR_AS(fi, FasterImmediate);
	REG(fi.get_rd()) = fi.signed_imm();
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_MV, rv32i_mv) {
	VIEW_INSTR_AS(fi, FasterMove);
	REG(fi.get_rd()) = REG(fi.get_rs1());
	NEXT_INSTR();
}
#ifdef RISCV_64I
INSTRUCTION(RV64I_BC_ADDIW, rv64i_addiw) {
	if constexpr (W >= 8) {
		VIEW_INSTR_AS(fi, FasterItype);
		REG(fi.get_rs1()) = (int32_t)
			((uint32_t)REG(fi.get_rs2()) + fi.signed_imm());
		NEXT_INSTR();
	}
	else UNUSED_FUNCTION();
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
	VIEW_INSTR_AS(fi, FasterItype);
	// SLLI: Logical left-shift 5/6/7-bit immediate
	REG(fi.get_rs1()) =
		REG(fi.get_rs2()) << fi.unsigned_imm();
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_SLTI, rv32i_slti) {
	VIEW_INSTR_AS(fi, FasterItype);
	// SLTI: Set less than immediate
	REG(fi.get_rs1()) = (saddr_t(REG(fi.get_rs2())) < fi.signed_imm());
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_SLTIU, rv32i_sltiu) {
	VIEW_INSTR_AS(fi, FasterItype);
	// SLTIU: Sign-extend, then treat as unsigned
	REG(fi.get_rs1()) = (REG(fi.get_rs2()) < addr_t(fi.signed_imm()));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_XORI, rv32i_xori) {
	VIEW_INSTR_AS(fi, FasterItype);
	// XORI
	REG(fi.get_rs1()) = REG(fi.get_rs2()) ^ fi.signed_imm();
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_SRLI, rv32i_srli) {
	VIEW_INSTR_AS(fi, FasterItype);
	// SRLI: Shift-right logical 5/6/7-bit immediate
	REG(fi.get_rs1()) = REG(fi.get_rs2()) >> fi.unsigned_imm();
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_SRAI, rv32i_srai) {
	VIEW_INSTR_AS(fi, FasterItype);
	// SRAI: Shift-right arithmetical (preserve the sign bit)
	REG(fi.get_rs1()) = saddr_t(REG(fi.get_rs2())) >> fi.unsigned_imm();
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_ORI, rv32i_ori) {
	VIEW_INSTR_AS(fi, FasterItype);
	// ORI: Or sign-extended 12-bit immediate
	REG(fi.get_rs1()) = REG(fi.get_rs2()) | fi.signed_imm();
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_ANDI, rv32i_andi) {
	VIEW_INSTR_AS(fi, FasterItype);
	// ANDI: And sign-extended 12-bit immediate
	REG(fi.get_rs1()) = REG(fi.get_rs2()) & fi.signed_imm();
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
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = REG(fi.get_rs2()) + fi.signed_imm();
	REG(fi.get_rs1()) =
		(int32_t)CPU().memory().template read<uint32_t>(addr);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_STW, rv32i_stw) {
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr  = REG(fi.get_rs1()) + fi.signed_imm();
	CPU().memory().template write<uint32_t>(addr, REG(fi.get_rs2()));
	NEXT_INSTR();
}
#ifdef RISCV_64I
INSTRUCTION(RV32I_BC_LDD, rv32i_ldd) {
	if constexpr (W >= 8) {
		VIEW_INSTR_AS(fi, FasterItype);
		const auto addr = REG(fi.get_rs2()) + fi.signed_imm();
		REG(fi.get_rs1()) =
			(int64_t)CPU().memory().template read<uint64_t>(addr);
		NEXT_INSTR();
	}
	else UNUSED_FUNCTION();
}
INSTRUCTION(RV32I_BC_STD, rv32i_std) {
	if constexpr (W >= 8) {
		VIEW_INSTR_AS(fi, FasterItype);
		const auto addr  = REG(fi.get_rs1()) + fi.signed_imm();
		CPU().memory().template write<uint64_t>(addr, REG(fi.get_rs2()));
		NEXT_INSTR();
	}
	else UNUSED_FUNCTION();
}
#endif // RISCV_64I

INSTRUCTION(RV32F_BC_FLW, rv32i_flw) {
	VIEW_INSTR_AS(fi, FasterItype);
	auto addr = REG(fi.rs2) + fi.signed_imm();
	auto& dst = REGISTERS().getfl(fi.rs1);
	dst.load_u32(CPU().memory().template read<uint32_t> (addr));
	NEXT_INSTR();
}
INSTRUCTION(RV32F_BC_FLD, rv32i_fld) {
	VIEW_INSTR_AS(fi, FasterItype);
	auto addr = REG(fi.rs2) + fi.signed_imm();
	auto& dst = REGISTERS().getfl(fi.rs1);
	dst.load_u64(CPU().memory().template read<uint64_t> (addr));
	NEXT_INSTR();
}

#ifdef RISCV_EXT_COMPRESSED
INSTRUCTION(RV32C_BC_LDD, rv32c_ldd) {
	if constexpr (W >= 8) {
		VIEW_INSTR_AS(fi, FasterItype);
		const auto addr = REG(fi.get_rs2()) + fi.signed_imm();
		REG(fi.get_rs1()) =
			(int64_t)CPU().memory().template read<uint64_t>(addr);
		NEXT_C_INSTR();
	}
	else UNUSED_FUNCTION();
}
INSTRUCTION(RV32C_BC_STD, rv32c_std) {
	if constexpr (W >= 8) {
		VIEW_INSTR_AS(fi, FasterItype);
		const auto addr = REG(fi.get_rs1()) + fi.signed_imm();
		CPU().memory().template write<uint64_t>(addr, REG(fi.get_rs2()));
		NEXT_C_INSTR();
	}
	else UNUSED_FUNCTION();
}
INSTRUCTION(RV32C_BC_LDW, rv32c_ldw) {
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = REG(fi.get_rs2()) + fi.signed_imm();
	REG(fi.get_rs1()) =
		(int32_t)CPU().memory().template read<uint32_t>(addr);
	NEXT_C_INSTR();
}
INSTRUCTION(RV32C_BC_STW, rv32c_stw) {
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = REG(fi.get_rs1()) + fi.signed_imm();
	CPU().memory().template write<uint32_t>(addr, REG(fi.get_rs2()));
	NEXT_C_INSTR();
}
#endif // RISCV_EXT_COMPRESSED


INSTRUCTION(RV32I_BC_AUIPC, rv32i_auipc)
{
	VIEW_INSTR_AS(fi, FasterJtype);
	// AUIPC using re-constructed PC
	//REG(instr.Utype.rd) = (pc - DECODER().block_bytes()) + instr.Utype.upper_imm();
	REG(fi.rd) = (pc - DECODER().block_bytes()) + fi.upper_imm();
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_LUI, rv32i_lui)
{
	VIEW_INSTR_AS(fi, FasterJtype);
	REG(fi.rd) = fi.upper_imm();
	NEXT_INSTR();
}

#define OP_INSTR()                       \
	VIEW_INSTR_AS(fi, FasterOpType);     \
	auto& dst = REG(fi.get_rd());        \
	const auto src1 = REG(fi.get_rs1()); \
	const auto src2 = REG(fi.get_rs2());

INSTRUCTION(RV32I_BC_OP_ADD, rv32i_op_add) {
	OP_INSTR();
	dst = src1 + src2;
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_SUB, rv32i_op_sub) {
	OP_INSTR();
	dst = src1 - src2;
	NEXT_INSTR();
}

INSTRUCTION(RV32I_BC_OP_SLL, rv32i_op_sll) {
	OP_INSTR();
	dst = src1 << (src2 & (XLEN - 1));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_SLT, rv32i_op_slt) {
	OP_INSTR();
	dst = (saddr_t(src1) < saddr_t(src2));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_SLTU, rv32i_op_sltu) {
	OP_INSTR();
	dst = (src1 < src2);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_XOR, rv32i_op_xor) {
	OP_INSTR();
	dst = src1 ^ src2;
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_SRL, rv32i_op_srl) {
	OP_INSTR();
	dst = src1 >> (src2 & (XLEN - 1));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_OR, rv32i_op_or) {
	OP_INSTR();
	dst = src1 | src2;
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_AND, rv32i_op_and) {
	OP_INSTR();
	dst = src1 & src2;
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_MUL, rv32i_op_mul) {
	OP_INSTR();
	// MUL keeps the low XLEN bits, which is the same product either way, but
	// only the unsigned multiplication is defined when it overflows
	dst = addr_t(src1) * addr_t(src2);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_SH1ADD, rv32i_op_sh1add) {
	OP_INSTR();
	dst = src2 + (src1 << 1);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_SH2ADD, rv32i_op_sh2add) {
	OP_INSTR();
	dst = src2 + (src1 << 2);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_SH3ADD, rv32i_op_sh3add) {
	OP_INSTR();
	dst = src2 + (src1 << 3);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_SRA, rv32i_op_sra) {
	OP_INSTR();
	dst = saddr_t(src1) >> (src2 & (XLEN-1));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_ZEXT_H, rv32i_op_zext_h) {
	OP_INSTR();
	dst = uint16_t(src1);
	(void)src2;
	NEXT_INSTR();
}

#ifdef RISCV_64I
INSTRUCTION(RV64I_BC_OP_ADDW, rv64i_op_addw) {
	if constexpr (W >= 8) {
		OP_INSTR();
		dst = int32_t(uint32_t(src1) + uint32_t(src2));
		NEXT_INSTR();
	}
	else UNUSED_FUNCTION();
}
INSTRUCTION(RV64I_BC_OP_SUBW, rv64i_op_subw) {
	if constexpr (W >= 8) {
		OP_INSTR();
		dst = int32_t(uint32_t(src1) - uint32_t(src2));
		NEXT_INSTR();
	}
	else UNUSED_FUNCTION();
}
INSTRUCTION(RV64I_BC_OP_MULW, rv64i_op_mulw) {
	if constexpr (W >= 8) {
		OP_INSTR();
		dst = int32_t(uint32_t(src1) * uint32_t(src2));
		NEXT_INSTR();
	}
	else UNUSED_FUNCTION();
}
INSTRUCTION(RV64I_BC_OP_ADD_UW, rv64i_op_add_uw) {
	if constexpr (W >= 8) {
		OP_INSTR();
		dst = uint32_t(src1) + src2;
		NEXT_INSTR();
	}
	else UNUSED_FUNCTION();
}
INSTRUCTION(RV64I_BC_SLLIW, rv64i_slliw) {
	if constexpr (W >= 8) {
		VIEW_INSTR_AS(fi, FasterItype);
		REG(fi.get_rs1()) = (int32_t)
			((uint32_t)REG(fi.get_rs2()) << fi.imm);
		NEXT_INSTR();
	}
	else UNUSED_FUNCTION();
}
INSTRUCTION(RV64I_BC_SRLIW, rv64i_srliw) {
	if constexpr (W >= 8) {
		VIEW_INSTR_AS(fi, FasterItype);
		REG(fi.get_rs1()) = (int32_t)
			((uint32_t)REG(fi.get_rs2()) >> fi.imm);
		NEXT_INSTR();
	}
	else UNUSED_FUNCTION();
}
#endif // RISCV_64I

INSTRUCTION(RV32I_BC_SEXT_B, rv32i_sext_b) {
	VIEW_INSTR_AS(fi, FasterItype);
	REG(fi.get_rs1()) = saddr_t(int8_t(REG(fi.get_rs2())));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_SEXT_H, rv32i_sext_h) {
	VIEW_INSTR_AS(fi, FasterItype);
	REG(fi.get_rs1()) = saddr_t(int16_t(REG(fi.get_rs2())));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_BSETI, rv32i_bseti) {
	VIEW_INSTR_AS(fi, FasterItype);
	// BSETI: Bit-set immediate
	REG(fi.get_rs1()) =
		REG(fi.get_rs2()) | (addr_t(1) << fi.unsigned_imm());
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_BEXTI, rv32i_bexti) {
	VIEW_INSTR_AS(fi, FasterItype);
	// BEXTI: Single-bit Extract
	REG(fi.get_rs1()) =
		(REG(fi.get_rs2()) >> fi.unsigned_imm()) & 1;
	NEXT_INSTR();
}

INSTRUCTION(RV32F_BC_FSW, rv32i_fsw) {
	VIEW_INSTR_AS(fi, FasterItype);
	const auto& src = REGISTERS().getfl(fi.rs2);
	auto addr = REG(fi.rs1) + fi.signed_imm();
	CPU().memory().template write<uint32_t> (addr, src.i32[0]);
	NEXT_INSTR();
}
INSTRUCTION(RV32F_BC_FSD, rv32i_fsd) {
	VIEW_INSTR_AS(fi, FasterItype);
	const auto& src = REGISTERS().getfl(fi.rs2);
	auto addr = REG(fi.rs1) + fi.signed_imm();
	CPU().memory().template write<uint64_t> (addr, src.i64);
	NEXT_INSTR();
}
// Spec §11.3: an FP operation that produces a NaN must deliver the canonical
// quiet NaN, not the payload-propagating one a host FPU hands us. Guests that
// care are the ones that also want fflags, so the canonicalization rides along
// with FCSR emulation: without it a scripting guest would have to read the raw
// bits back as an integer to tell the difference, and the branch is not worth
// paying for on every single FP instruction.
#define SET_FLOAT_CANON(dst, expr) \
	if constexpr (fcsr_emulation) { \
		const float fcanon_r = (expr); \
		if (UNLIKELY(std::isnan(fcanon_r))) dst.load_u32(0x7FC00000u); \
		else dst.set_float(fcanon_r); \
	} else { \
		dst.set_float(expr); \
	}
#define SET_DOUBLE_CANON(dst, expr) \
	if constexpr (fcsr_emulation) { \
		const double dcanon_r = (expr); \
		if (UNLIKELY(std::isnan(dcanon_r))) dst.load_u64(0x7FF8000000000000ull); \
		else dst.f64 = dcanon_r; \
	} else { \
		dst.f64 = (expr); \
	}

INSTRUCTION(RV32F_BC_FADD, rv32f_fadd) {
	VIEW_INSTR_AS(fi, FasterFloatType);
	#define FLREGS() \
		auto& dst = REGISTERS().getfl(fi.get_rd()); \
		const auto& rs1 = REGISTERS().getfl(fi.get_rs1()); \
		const auto& rs2 = REGISTERS().getfl(fi.get_rs2());
	FLREGS();
	if (fi.func == 0x0)
	{ // float32
		if constexpr (W >= 8 && nanboxing) {
			if (UNLIKELY(static_cast<uint32_t>(rs1.i32[1]) != 0xFFFFFFFFu
				|| static_cast<uint32_t>(rs2.i32[1]) != 0xFFFFFFFFu)) {
				dst.load_u32(0x7FC00000u);
				NEXT_INSTR();
			}
		}
		if constexpr (fcsr_emulation) {
			// The operands are read out before the store, because rd is
			// allowed to alias rs1 or rs2. NX when the exact sum (computed on
			// operands widened to double) does not round-trip to the single-
			// precision result.
			const float fa = rs1.f32[0], fb = rs2.f32[0];
			const double exact = (double)fa + (double)fb;
			SET_FLOAT_CANON(dst, fa + fb);
			if ((double)(fa + fb) != exact) REGISTERS().fcsr().fflags |= 1;
		} else {
			SET_FLOAT_CANON(dst, rs1.f32[0] + rs2.f32[0]);
		}
	}
	else
	{ // float64
		SET_DOUBLE_CANON(dst, rs1.f64 + rs2.f64);
	}
	NEXT_INSTR();
}
INSTRUCTION(RV32F_BC_FSUB, rv32f_fsub) {
	VIEW_INSTR_AS(fi, FasterFloatType);
	FLREGS();
	if (fi.func == 0x0)
	{ // float32
		if constexpr (W >= 8 && nanboxing) {
			if (UNLIKELY(static_cast<uint32_t>(rs1.i32[1]) != 0xFFFFFFFFu
				|| static_cast<uint32_t>(rs2.i32[1]) != 0xFFFFFFFFu)) {
				dst.load_u32(0x7FC00000u);
				NEXT_INSTR();
			}
		}
		if constexpr (fcsr_emulation) {
			// Operands read out before the store: rd may alias rs1 or rs2.
			const float fa = rs1.f32[0], fb = rs2.f32[0];
			const double exact = (double)fa - (double)fb;
			SET_FLOAT_CANON(dst, fa - fb);
			if ((double)(fa - fb) != exact) REGISTERS().fcsr().fflags |= 1;
		} else {
			SET_FLOAT_CANON(dst, rs1.f32[0] - rs2.f32[0]);
		}
	}
	else
	{ // float64
		SET_DOUBLE_CANON(dst, rs1.f64 - rs2.f64);
	}
	NEXT_INSTR();
}
INSTRUCTION(RV32F_BC_FMUL, rv32f_fmul) {
	VIEW_INSTR_AS(fi, FasterFloatType);
	FLREGS();
	if (fi.func == 0x0)
	{ // float32
		if constexpr (W >= 8 && nanboxing) {
			if (UNLIKELY(static_cast<uint32_t>(rs1.i32[1]) != 0xFFFFFFFFu
				|| static_cast<uint32_t>(rs2.i32[1]) != 0xFFFFFFFFu)) {
				dst.load_u32(0x7FC00000u);
				NEXT_INSTR();
			}
		}
		if constexpr (fcsr_emulation) {
			// The operands are read out before the store, because rd is
			// allowed to alias rs1 or rs2.
			const uint32_t ia = rs1.i32[0], ib = rs2.i32[0];
			const float fa = rs1.f32[0], fb = rs2.f32[0];
			SET_FLOAT_CANON(dst, fa * fb);
			// Finite inputs that produce an infinity overflowed; finite inputs
			// that produce an inexact subnormal underflowed. Both imply NX.
			if ((ia & 0x7f800000u) != 0x7f800000u
				&& (ib & 0x7f800000u) != 0x7f800000u) {
				const uint32_t result = dst.i32[0] & 0x7fffffffu;
				if (result == 0x7f800000u) {
					CPU().registers().fcsr().fflags |= 5; // OF | NX
					// The overflow value depends on the rounding mode (IEEE 754
					// §7.4): RTZ saturates to the largest finite number for
					// either sign, RDN only for a positive result and RUP only
					// for a negative one. RNE and RMM keep the host infinity.
					const unsigned rm = CPU().registers().fcsr().frm;
					const bool neg = (dst.i32[0] & 0x80000000u) != 0;
					if (rm == 0x1 || (rm == 0x2 && !neg) || (rm == 0x3 && neg))
						dst.i32[0] = (dst.i32[0] & 0x80000000u) | 0x7F7FFFFFu;
				}
				else if (result < 0x00800000u
					&& (double)fa * (double)fb != dst.f32[0])
					CPU().registers().fcsr().fflags |= 3; // UF | NX
			}
		} else {
			dst.set_float(rs1.f32[0] * rs2.f32[0]);
		}
	}
	else
	{ // float64
		SET_DOUBLE_CANON(dst, rs1.f64 * rs2.f64);
	}
	NEXT_INSTR();
}
INSTRUCTION(RV32F_BC_FDIV, rv32f_fdiv) {
	VIEW_INSTR_AS(fi, FasterFloatType);
	FLREGS();
	if (fi.func == 0x0)
	{ // float32
		if constexpr (W >= 8 && nanboxing) {
			if (UNLIKELY(static_cast<uint32_t>(rs1.i32[1]) != 0xFFFFFFFFu
				|| static_cast<uint32_t>(rs2.i32[1]) != 0xFFFFFFFFu)) {
				dst.load_u32(0x7FC00000u);
				NEXT_INSTR();
			}
		}
		if constexpr (fcsr_emulation) {
			// Operands read out before the store: rd may alias rs1 or rs2.
			// DZ is only for a finite non-zero numerator over zero: 0/0 is NV
			// and inf/0 is exact.
			const uint32_t ia = rs1.i32[0], ib = rs2.i32[0];
			const float fa = rs1.f32[0], fb = rs2.f32[0];
			const float fr = fa / fb;
			const double exact = (double)fa / (double)fb;
			SET_FLOAT_CANON(dst, fr);
			if (fr != fr) {
				if ((ia & 0x7fffffffu) == 0 && (ib & 0x7fffffffu) == 0)
					CPU().registers().fcsr().fflags |= 16; // NV
				else if ((ia & 0x7f800000u) == 0x7f800000u
					&& (ib & 0x7f800000u) == 0x7f800000u)
					CPU().registers().fcsr().fflags |= 16; // NV
			} else {
				if ((ia & 0x7fffffffu) != 0 && (ia & 0x7f800000u) != 0x7f800000u
					&& (ib & 0x7fffffffu) == 0)
					CPU().registers().fcsr().fflags |= 8; // DZ
				if ((double)fr != exact)
					CPU().registers().fcsr().fflags |= 1; // NX
				const uint32_t ir = dst.i32[0] & 0x7fffffffu;
				if ((ib & 0x7fffffffu) != 0 && (ia & 0x7f800000u) != 0x7f800000u
					&& ir == 0x7f800000u)
					CPU().registers().fcsr().fflags |= 5; // OF | NX
				else if (ir < 0x00800000u && (double)fr != exact)
					CPU().registers().fcsr().fflags |= 3; // UF | NX
			}
		} else {
			dst.set_float(rs1.f32[0] / rs2.f32[0]);
		}
	}
	else
	{ // float64
		if constexpr (fcsr_emulation) {
			// Operands read out before the store: rd may alias rs1 or rs2.
			const uint64_t ia = rs1.i64, ib = rs2.i64;
			const double da = rs1.f64, db = rs2.f64;
			const double dr = da / db;
			const long double exact = (long double)da / (long double)db;
			SET_DOUBLE_CANON(dst, dr);
			if (dr != dr) {
				if ((ia & 0x7fffffffffffffffull) == 0 && (ib & 0x7fffffffffffffffull) == 0)
					CPU().registers().fcsr().fflags |= 16; // NV
				else if ((ia & 0x7ff0000000000000ull) == 0x7ff0000000000000ull
					&& (ib & 0x7ff0000000000000ull) == 0x7ff0000000000000ull)
					CPU().registers().fcsr().fflags |= 16; // NV
			} else {
				if ((ia & 0x7fffffffffffffffull) != 0
					&& (ia & 0x7ff0000000000000ull) != 0x7ff0000000000000ull
					&& (ib & 0x7fffffffffffffffull) == 0)
					CPU().registers().fcsr().fflags |= 8; // DZ
				if ((long double)dr != exact)
					CPU().registers().fcsr().fflags |= 1; // NX
				const uint64_t ir = dst.i64 & 0x7fffffffffffffffull;
				if ((ib & 0x7fffffffffffffffull) != 0
					&& (ia & 0x7ff0000000000000ull) != 0x7ff0000000000000ull
					&& ir == 0x7ff0000000000000ull)
					CPU().registers().fcsr().fflags |= 5; // OF | NX
				else if (ir < 0x0010000000000000ull && (long double)dr != exact)
					CPU().registers().fcsr().fflags |= 3; // UF | NX
			}
		} else {
			dst.f64 = rs1.f64 / rs2.f64;
		}
	}
	NEXT_INSTR();
}
INSTRUCTION(RV32F_BC_FMADD, rv32f_fmadd) {
	VIEW_INSTR_AS(fi, rv32f_instruction);
	#define FMAREGS() \
		auto& dst = REGISTERS().getfl(fi.R4type.rd);  \
		auto& rs1 = REGISTERS().getfl(fi.R4type.rs1); \
		auto& rs2 = REGISTERS().getfl(fi.R4type.rs2); \
		auto& rs3 = REGISTERS().getfl(fi.R4type.rs3);
	FMAREGS();
	// RISC-V spec §11.6: FMA must round only once. std::fma is the
	// IEEE 754 fused multiply-add (single rounding). Prior code used
	// `a * b + c`, which does two roundings — spec violation, visible
	// as 1-ULP divergences in the fuzz oracle.
	//
	// Spec §11.3: a NaN result must be the canonical qNaN. Only under FCSR
	// emulation, same as the other arithmetic bytecodes.
	if (fi.R4type.funct2 == 0x0) { // float32
		if constexpr (W >= 8 && nanboxing) {
			if (UNLIKELY(static_cast<uint32_t>(rs1.i32[1]) != 0xFFFFFFFFu
				|| static_cast<uint32_t>(rs2.i32[1]) != 0xFFFFFFFFu
				|| static_cast<uint32_t>(rs3.i32[1]) != 0xFFFFFFFFu)) {
				dst.load_u32(0x7FC00000u);
				NEXT_INSTR();
			}
		}
		SET_FLOAT_CANON(dst, std::fma(rs1.f32[0], rs2.f32[0], rs3.f32[0]));
	} else if (fi.R4type.funct2 == 0x1) { // float64
		SET_DOUBLE_CANON(dst, std::fma(rs1.f64, rs2.f64, rs3.f64));
	}
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
	if constexpr (W >= 8) {
		VIEW_INSTR_AS(fi, FasterItype);
		//dst = (int32_t)src >> instr.Itype.shift_imm();
		REG(fi.get_rs1()) =
			(int32_t)REG(fi.get_rs2()) >> fi.imm;
		NEXT_INSTR();
	}
	else UNUSED_FUNCTION();
}
INSTRUCTION(RV64I_BC_OP_SH1ADD_UW, rv64i_op_sh1add_uw) {
	if constexpr (W >= 8) {
		OP_INSTR();
		dst = src2 + (addr_t(uint32_t(src1)) << 1);
		NEXT_INSTR();
	}
	else UNUSED_FUNCTION();
}
INSTRUCTION(RV64I_BC_OP_SH2ADD_UW, rv64i_op_sh2add_uw) {
	if constexpr (W >= 8) {
		OP_INSTR();
		dst = src2 + (addr_t(uint32_t(src1)) << 2);
		NEXT_INSTR();
	}
	else UNUSED_FUNCTION();
}
#endif // RISCV_64I

INSTRUCTION(RV32I_BC_OP_DIV, rv32i_op_div) {
	OP_INSTR();
	dst = rv_div<addr_t>(src1, src2);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_DIVU, rv32i_op_divu) {
	OP_INSTR();
	if (LIKELY(src2 != 0)) {
		dst = src1 / src2;
	} else {
		dst = addr_t(-1);
	}
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_REM, rv32i_op_rem) {
	OP_INSTR();
	dst = rv_rem<addr_t>(src1, src2);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_REMU, rv32i_op_remu) {
	OP_INSTR();
	if (LIKELY(src2 != 0)) {
		dst = src1 % src2;
	} else {
		dst = src1;
	}
	NEXT_INSTR();
}

INSTRUCTION(RV32I_BC_LDB, rv32i_ldb) {
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = REG(fi.get_rs2()) + fi.signed_imm();
	REG(fi.get_rs1()) =
		int8_t(CPU().memory().template read<uint8_t>(addr));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_LDBU, rv32i_ldbu) {
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = REG(fi.get_rs2()) + fi.signed_imm();
	REG(fi.get_rs1()) =
		saddr_t(CPU().memory().template read<uint8_t>(addr));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_LDH, rv32i_ldh) {
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = REG(fi.get_rs2()) + fi.signed_imm();
	REG(fi.get_rs1()) =
		int16_t(CPU().memory().template read<uint16_t>(addr));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_LDHU, rv32i_ldhu) {
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = REG(fi.get_rs2()) + fi.signed_imm();
	REG(fi.get_rs1()) =
		saddr_t(CPU().memory().template read<uint16_t>(addr));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_STB, rv32i_stb) {
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = REG(fi.get_rs1()) + fi.signed_imm();
	CPU().memory().template write<uint8_t>(addr, REG(fi.get_rs2()));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_STH, rv32i_sth) {
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = REG(fi.get_rs1()) + fi.signed_imm();
	CPU().memory().template write<uint16_t>(addr, REG(fi.get_rs2()));
	NEXT_INSTR();
}
#ifdef RISCV_64I
INSTRUCTION(RV32I_BC_LDWU, rv32i_ldwu) {
	if constexpr (W >= 8) {
		VIEW_INSTR_AS(fi, FasterItype);
		const auto addr = REG(fi.get_rs2()) + fi.signed_imm();
		REG(fi.get_rs1()) =
			CPU().memory().template read<uint32_t>(addr);
		NEXT_INSTR();
	}
	else UNUSED_FUNCTION();
}
#endif // RISCV_64I

#ifdef RISCV_EXT_COMPRESSED
INSTRUCTION(RV32C_BC_SRLI, rv32c_srli) {
	VIEW_INSTR_AS(fi, FasterItype);
	REG(fi.get_rs1()) >>= fi.imm;
	NEXT_C_INSTR();
}
INSTRUCTION(RV32C_BC_ADD, rv32c_add) {
	VIEW_INSTR_AS(fi, FasterItype);
	REG(fi.get_rs1()) += REG(fi.get_rs2());
	NEXT_C_INSTR();
}
INSTRUCTION(RV32C_BC_XOR, rv32c_xor) {
	VIEW_INSTR_AS(fi, FasterItype);
	REG(fi.get_rs1()) ^= REG(fi.get_rs2());
	NEXT_C_INSTR();
}
INSTRUCTION(RV32C_BC_OR, rv32c_or) {
	VIEW_INSTR_AS(fi, FasterItype);
	REG(fi.get_rs1()) |= REG(fi.get_rs2());
	NEXT_C_INSTR();
}
INSTRUCTION(RV32C_BC_ANDI, rv32c_andi) {
	VIEW_INSTR_AS(fi, FasterItype);
	REG(fi.get_rs1()) &= fi.signed_imm();
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
	switch (DECODER().m_handler) {
	case 0: { // Live-patch native code (binary translation or asmjit)
#if defined(RISCV_BINARY_TRANSLATION) || defined(RISCV_ASMJIT)
		// Special bytecode that does not read any decoder data
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
		// 3. Execute the instruction
		EXECUTE_CURRENT();
#else
		// Invalid handler
		DECODER().set_bytecode(RV32I_BC_INVALID);
		DECODER().set_invalid_handler();
#endif
	}	break;
	case 1: { // Live-patch JALR -> STOP
		// Check if RA == memory exit address
		if (RISCV_SPECSAFE(REG(REG_RA) == MACHINE().memory.exit_address())) {
			// Hot-swap the bytecode to a STOP
			DECODER().set_bytecode(RV32I_BC_STOP);
			EXECUTE_CURRENT();
		}
		// Otherwise, leave the JALR instruction as is (NOTE: sets invalid handler)
		DECODER().set_atomic_bytecode_and_handler(RV32I_BC_JALR, 0);
	}	break;
#ifdef RISCV_EXT_COMPRESSED
	case 2: { // Live-patch C.JR -> STOP
		// Check if RA == memory exit address
		if (RISCV_SPECSAFE(REG(REG_RA) == MACHINE().memory.exit_address())) {
			// Hot-swap the bytecode to a STOP
			DECODER().set_bytecode(RV32I_BC_STOP);
			EXECUTE_CURRENT();
		}
		// Otherwise, leave the JR instruction as is (NOTE: sets invalid handler)
		DECODER().set_atomic_bytecode_and_handler(RV32C_BC_JR, 0);
	}	break;
#endif
	default:
		// Invalid handler
		DECODER().set_bytecode(RV32I_BC_INVALID);
		DECODER().set_invalid_handler();
	}
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
