
#ifdef BYTECODES_OP_IMM

#ifdef RISCV_EXT_COMPRESSED
INSTRUCTION(RV32C_BC_ADDI, rv32c_addi) {
	VIEW_INSTR_AS(fi, FasterItype);
	REG(fi.get_rs1()) = REG(fi.get_rs2()) + fi.signed_imm();
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
INSTRUCTION(RV64I_BC_ADDIW, rv64i_addiw) {
	if constexpr (W >= 8) {
        VIEW_INSTR_AS(fi, FasterItype);
        REG(fi.get_rs1()) = (int32_t)
            ((uint32_t)REG(fi.get_rs2()) + fi.signed_imm());
        NEXT_INSTR();
	}
#ifdef DISPATCH_MODE_TAILCALL
    else UNUSED_FUNCTION();
#endif
}
INSTRUCTION(RV32I_BC_SLLI, rv32i_slli) {
	VIEW_INSTR_AS(fi, FasterItype);
	// SLLI: Logical left-shift 5/6/7-bit immediate
	REG(fi.get_rs1()) =
		REG(fi.get_rs2()) << (fi.unsigned_imm() & (XLEN - 1));
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
	REG(fi.get_rs1()) = REG(fi.get_rs2()) >> (fi.unsigned_imm() & (XLEN - 1));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_SRAI, rv32i_srai) {
	VIEW_INSTR_AS(fi, FasterItype);
	// SRAI: Shift-right arithmetical (preserve the sign bit)
	REG(fi.get_rs1()) = saddr_t(REG(fi.get_rs2())) >> (fi.unsigned_imm() & (XLEN - 1));
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

#endif // OP_IMM

#ifdef BYTECODES_BRANCH

INSTRUCTION(RV32I_BC_BEQ, rv32i_beq) {
	VIEW_INSTR_AS(fi, FasterItype);
	if (REG(fi.get_rs1()) == REG(fi.get_rs2())) {
		PERFORM_BRANCH();
	}
	NEXT_BLOCK(4);
}
INSTRUCTION(RV32I_BC_BNE, rv32i_bne) {
	VIEW_INSTR_AS(fi, FasterItype);
	if (REG(fi.get_rs1()) != REG(fi.get_rs2())) {
#ifndef DISPATCH_MODE_TAILCALL
		if constexpr (ENABLE_FAST_BRANCH) {
			pc += fi.signed_imm();
			// XXX: This is a hand-written fast-path
			// Intentionally put everything after branch
			// TODO: Macro-ize the compressed_enabled constants
			if (LIKELY(!counter.overflowed())) {
				decoder += fi.signed_imm() / (compressed_enabled ? 2 : 4);
				counter.increment_counter(decoder->instruction_count());
				pc += decoder->block_bytes();
#ifdef DISPATCH_MODE_SWITCH_BASED
				break;
#else
				goto *computed_opcode[decoder->get_bytecode()];
#endif
			}
			goto check_jump;
		} else {
            PERFORM_BRANCH();
        }
#else
        PERFORM_BRANCH();
#endif
    }
    NEXT_BLOCK(4);
}
INSTRUCTION(RV32I_BC_BEQ_FW, rv32i_beq_fw) {
	VIEW_INSTR_AS(fi, FasterItype);
	if (REG(fi.get_rs1()) == REG(fi.get_rs2())) {
		PERFORM_FORWARD_BRANCH();
	}
	NEXT_BLOCK(4);
}
INSTRUCTION(RV32I_BC_BNE_FW, rv32i_bne_fw) {
	VIEW_INSTR_AS(fi, FasterItype);
	if (REG(fi.get_rs1()) != REG(fi.get_rs2())) {
		PERFORM_FORWARD_BRANCH();
	}
	NEXT_BLOCK(4);
}
INSTRUCTION(RV32I_BC_BLT, rv32i_blt) {
	VIEW_INSTR_AS(fi, FasterItype);
	if ((saddr_t)REG(fi.get_rs1()) < (saddr_t)REG(fi.get_rs2())) {
		PERFORM_BRANCH();
	}
	NEXT_BLOCK(4);
}
INSTRUCTION(RV32I_BC_BGE, rv32i_bge) {
	VIEW_INSTR_AS(fi, FasterItype);
	if ((saddr_t)REG(fi.get_rs1()) >= (saddr_t)REG(fi.get_rs2())) {
		PERFORM_BRANCH();
	}
	NEXT_BLOCK(4);
}
INSTRUCTION(RV32I_BC_BLTU, rv32i_bltu) {
	VIEW_INSTR_AS(fi, FasterItype);
	if (REG(fi.get_rs1()) < REG(fi.get_rs2())) {
		PERFORM_BRANCH();
	}
	NEXT_BLOCK(4);
}
INSTRUCTION(RV32I_BC_BGEU, rv32i_bgeu) {
	VIEW_INSTR_AS(fi, FasterItype);
	if (REG(fi.get_rs1()) >= REG(fi.get_rs2())) {
		PERFORM_BRANCH();
	}
	NEXT_BLOCK(4);
}

#endif // BRANCH

#ifdef BYTECODES_LOAD_STORE

INSTRUCTION(RV32I_BC_LDW, rv32i_ldw) {
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = REG(fi.get_rs2()) + fi.signed_imm();
	REG(fi.get_rs1()) =
		(int32_t)MACHINE().memory.template read<uint32_t>(addr);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_STW, rv32i_stw) {
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr  = REG(fi.get_rs1()) + fi.signed_imm();
	MACHINE().memory.template write<uint32_t>(addr, REG(fi.get_rs2()));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_LDWU, rv32i_ldwu) {
	if constexpr (W >= 8) {
		VIEW_INSTR_AS(fi, FasterItype);
		const auto addr = REG(fi.get_rs2()) + fi.signed_imm();
		REG(fi.get_rs1()) =
			MACHINE().memory.template read<uint32_t>(addr);
		NEXT_INSTR();
	}
#ifdef DISPATCH_MODE_TAILCALL
    else UNUSED_FUNCTION();
#endif
}
INSTRUCTION(RV32I_BC_LDD, rv32i_ldd) {
	if constexpr (W >= 8) {
		VIEW_INSTR_AS(fi, FasterItype);
		const auto addr = REG(fi.get_rs2()) + fi.signed_imm();
		REG(fi.get_rs1()) =
			(int64_t)MACHINE().memory.template read<uint64_t>(addr);
		NEXT_INSTR();
	}
#ifdef DISPATCH_MODE_TAILCALL
    else UNUSED_FUNCTION();
#endif
}
INSTRUCTION(RV32I_BC_STD, rv32i_std) {
	if constexpr (W >= 8) {
		VIEW_INSTR_AS(fi, FasterItype);
		const auto addr  = REG(fi.get_rs1()) + fi.signed_imm();
		MACHINE().memory.template write<uint64_t>(addr, REG(fi.get_rs2()));
		NEXT_INSTR();
	}
#ifdef DISPATCH_MODE_TAILCALL
    else UNUSED_FUNCTION();
#endif
}
INSTRUCTION(RV32I_BC_LDB, rv32i_ldb) {
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = REG(fi.get_rs2()) + fi.signed_imm();
	REG(fi.get_rs1()) =
		int8_t(MACHINE().memory.template read<uint8_t>(addr));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_LDBU, rv32i_ldbu) {
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = REG(fi.get_rs2()) + fi.signed_imm();
	REG(fi.get_rs1()) =
		saddr_t(MACHINE().memory.template read<uint8_t>(addr));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_LDH, rv32i_ldh) {
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = REG(fi.get_rs2()) + fi.signed_imm();
	REG(fi.get_rs1()) =
		int16_t(MACHINE().memory.template read<uint16_t>(addr));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_LDHU, rv32i_ldhu) {
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = REG(fi.get_rs2()) + fi.signed_imm();
	REG(fi.get_rs1()) =
		saddr_t(MACHINE().memory.template read<uint16_t>(addr));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_STB, rv32i_stb) {
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = REG(fi.get_rs1()) + fi.signed_imm();
	MACHINE().memory.template write<uint8_t>(addr, REG(fi.get_rs2()));
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_STH, rv32i_sth) {
	VIEW_INSTR_AS(fi, FasterItype);
	const auto addr = REG(fi.get_rs1()) + fi.signed_imm();
	MACHINE().memory.template write<uint16_t>(addr, REG(fi.get_rs2()));
	NEXT_INSTR();
}

INSTRUCTION(RV32F_BC_FLW, rv32i_flw) {
	VIEW_INSTR_AS(fi, rv32f_instruction);
	auto addr = REG(fi.Itype.rs1) + fi.Itype.signed_imm();
	auto& dst = REGISTERS().getfl(fi.Itype.rd);
	dst.load_u32(MACHINE().memory.template read<uint32_t> (addr));
	NEXT_INSTR();
}
INSTRUCTION(RV32F_BC_FLD, rv32i_fld) {
	VIEW_INSTR_AS(fi, rv32f_instruction);
	auto addr = REG(fi.Itype.rs1) + fi.Itype.signed_imm();
	auto& dst = REGISTERS().getfl(fi.Itype.rd);
	dst.load_u64(MACHINE().memory.template read<uint64_t> (addr));
	NEXT_INSTR();
}

#ifdef RISCV_EXT_COMPRESSED

INSTRUCTION(RV32C_BC_LDD, rv32c_ldd) {
	if constexpr (W >= 8) {
		VIEW_INSTR_AS(fi, FasterItype);
		const auto addr = REG(fi.get_rs2()) + fi.signed_imm();
		REG(fi.get_rs1()) =
			(int64_t)MACHINE().memory.template read<uint64_t>(addr);
		NEXT_C_INSTR();
	}
#ifdef DISPATCH_MODE_TAILCALL
    else UNUSED_FUNCTION();
#endif
}
INSTRUCTION(RV32C_BC_STD, rv32c_std) {
	if constexpr (W >= 8) {
		VIEW_INSTR_AS(fi, FasterItype);
		const auto addr = REG(fi.get_rs1()) + fi.signed_imm();
		MACHINE().memory.template write<uint64_t>(addr, REG(fi.get_rs2()));
		NEXT_C_INSTR();
	}
#ifdef DISPATCH_MODE_TAILCALL
    else UNUSED_FUNCTION();
#endif
}

#endif // RISCV_EXT_COMPRESSED

#endif // LOAD STORE

#ifdef BYTECODES_OP

INSTRUCTION(RV32I_BC_NOP, rv32i_nop)
{
    NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_AUIPC, rv32i_auipc)
{
    VIEW_INSTR();
    REG(instr.Utype.rd) = pc + instr.Utype.upper_imm();
    NEXT_BLOCK(4);
}
INSTRUCTION(RV32I_BC_LUI, rv32i_lui)
{
    VIEW_INSTR();
	REG(instr.Utype.rd) = instr.Utype.upper_imm();
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
	dst = saddr_t(src1) * saddr_t(src2);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_OP_DIV, rv32i_op_div) {
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
INSTRUCTION(RV32I_BC_OP_MULH, rv32i_op_mulh) {
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
INSTRUCTION(RV32I_BC_OP_MULHSU, rv32i_op_mulhsu) {
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
INSTRUCTION(RV32I_BC_OP_MULHU, rv32i_op_mulhu) {
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
INSTRUCTION(RV32I_BC_OP_REMU, rv32i_op_remu) {
	OP_INSTR();
	if (LIKELY(src2 != 0)) {
		dst = src1 % src2;
	} else {
		dst = addr_t(-1);
	}
	NEXT_INSTR();
}

#endif // OP

#ifdef BYTECODES_FLP

INSTRUCTION(RV32F_BC_FSW, rv32i_fsw) {
	VIEW_INSTR_AS(fi, rv32f_instruction);
	const auto& src = REGISTERS().getfl(fi.Stype.rs2);
	auto addr = REG(fi.Stype.rs1) + fi.Stype.signed_imm();
	MACHINE().memory.template write<uint32_t> (addr, src.i32[0]);
	NEXT_INSTR();
}
INSTRUCTION(RV32F_BC_FSD, rv32i_fsd) {
	VIEW_INSTR_AS(fi, rv32f_instruction);
	const auto& src = REGISTERS().getfl(fi.Stype.rs2);
	auto addr = REG(fi.Stype.rs1) + fi.Stype.signed_imm();
	MACHINE().memory.template write<uint64_t> (addr, src.i64);
	NEXT_INSTR();
}
INSTRUCTION(RV32F_BC_FADD, rv32f_fadd) {
	VIEW_INSTR_AS(fi, rv32f_instruction);
	#define FLREGS() \
		auto& dst = REGISTERS().getfl(fi.R4type.rd); \
		const auto& rs1 = REGISTERS().getfl(fi.R4type.rs1); \
		const auto& rs2 = REGISTERS().getfl(fi.R4type.rs2);
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
INSTRUCTION(RV32F_BC_FSUB, rv32f_fsub) {
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
INSTRUCTION(RV32F_BC_FMUL, rv32f_fmul) {
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
INSTRUCTION(RV32F_BC_FDIV, rv32f_fdiv) {
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
INSTRUCTION(RV32F_BC_FMADD, rv32f_fmadd) {
	VIEW_INSTR_AS(fi, rv32f_instruction);
	#define FMAREGS() \
		auto& dst = REGISTERS().getfl(fi.R4type.rd);  \
		auto& rs1 = REGISTERS().getfl(fi.R4type.rs1); \
		auto& rs2 = REGISTERS().getfl(fi.R4type.rs2); \
		auto& rs3 = REGISTERS().getfl(fi.R4type.rs3);
	FMAREGS();
	if (fi.R4type.funct2 == 0x0) { // float32
		dst.set_float(rs1.f32[0] * rs2.f32[0] + rs3.f32[0]);
	} else if (fi.R4type.funct2 == 0x1) { // float64
		dst.f64 = rs1.f64 * rs2.f64 + rs3.f64;
	}
	NEXT_INSTR();
}

#ifdef RISCV_EXT_VECTOR
INSTRUCTION(RV32V_BC_VLE32, rv32v_vle32) {
	VIEW_INSTR_AS(vi, rv32v_instruction);
	const auto addr = REG(vi.VLS.rs1) & ~addr_t(VectorLane::size()-1);
	REGISTERS().rvv().get(vi.VLS.vd) =
		MACHINE().memory.template read<VectorLane> (addr);
	NEXT_INSTR();
}
INSTRUCTION(RV32V_BC_VSE32, rv32v_vse32) {
	VIEW_INSTR_AS(vi, rv32v_instruction);
	const auto addr = REG(vi.VLS.rs1) & ~addr_t(VectorLane::size()-1);
	auto& dst = REGISTERS().rvv().get(vi.VLS.vd);
	MACHINE().memory.template write<VectorLane> (addr, dst);
	NEXT_INSTR();
}
INSTRUCTION(RV32V_BC_VFADD_VV, rv32v_vfadd_vv) {
	VIEW_INSTR_AS(vi, rv32v_instruction);
	auto& rvv = REGISTERS().rvv();
	for (size_t i = 0; i < rvv.f32(0).size(); i++) {
		rvv.f32(vi.OPVV.vd)[i] = rvv.f32(vi.OPVV.vs1)[i] + rvv.f32(vi.OPVV.vs2)[i];
	}
	NEXT_INSTR();
}
#endif // RISCV_EXT_VECTOR

#endif // FLP
