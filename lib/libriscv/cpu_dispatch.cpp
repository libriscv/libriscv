#include "machine.hpp"
#include "decoder_cache.hpp"
#include "instruction_counter.hpp"
#include "instruction_list.hpp"
#include "extern_instructions.hpp"
#include "threaded_bytecodes.hpp"
#include "rv32i_instr.hpp"
#include "rvfd.hpp"

namespace riscv
{
#define VIEW_INSTR() \
	auto instr = decoder->view_instr();
#define VIEW_INSTR_AS(name, x) \
	auto name = decoder->template view_instr<x>();
#define NEXT_INSTR()            \
	if constexpr (EXTRA_CHECKS) { \
		if (decoder->get_bytecode() < 0 || decoder->get_bytecode() >= OPCODES) \
			throw MachineException(ILLEGAL_OPERATION, "Jumping to illegal opcode"); \
	}	\
	goto *computed_opcode[(++decoder)->get_bytecode()];
#define NEXT_BLOCK(len) \
	pc += len; \
	decoder++; \
	goto continue_block;
#define PERFORM_BRANCH() \
	pc += instr.Btype.signed_imm(); \
	goto check_jump;
#define PERFORM_FAST_BRANCH() \
	pc += fi.signed_imm(); \
	goto check_jump;

template <int W> __attribute__((hot))
void CPU<W>::simulate_threaded(uint64_t imax)
{
	static constexpr bool EXTRA_CHECKS = false;
	static constexpr uint32_t XLEN = W * 8;
	using addr_t  = address_type<W>;
	using saddr_t = std::make_signed_t<addr_t>;

	static constexpr void *computed_opcode[] = {
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
		[RV32I_BC_SDB]     = &&rv32i_sdb,
		[RV32I_BC_SDH]     = &&rv32i_sdh,
		[RV32I_BC_SDW]     = &&rv32i_sdw,
		[RV32I_BC_BEQ]     = &&rv32i_beq,
		[RV32I_BC_BNE]     = &&rv32i_bne,
		[RV32I_BC_BLT]     = &&rv32i_blt,
		[RV32I_BC_BGE]     = &&rv32i_bge,
		[RV32I_BC_BLTU]    = &&rv32i_bltu,
		[RV32I_BC_BGEU]    = &&rv32i_bgeu,
		[RV32I_BC_JAL]     = &&rv32i_jal,
		[RV32I_BC_JALR]    = &&rv32i_jalr,
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
		[RV32I_BC_SYSCALL] = &&rv32i_syscall,
		[RV32I_BC_SYSTEM]  = &&rv32i_system,
		[RV32F_BC_FLW]     = &&rv32i_flw,
		[RV32F_BC_FLD]     = &&rv32i_fld,
		[RV32F_BC_FSW]     = &&rv32i_fsw,
		[RV32F_BC_FSD]     = &&rv32i_fsd,
		[RV32F_BC_FPFUNC]  = &&rv32f_fpfunc,
		[RV32F_BC_FMADD]   = &&rv32f_fmadd,
		[RV32F_BC_FMSUB]   = &&rv32f_fmsub,
		[RV32F_BC_FNMADD]  = &&rv32f_fnmadd,
		[RV32F_BC_FNMSUB]  = &&rv32f_fnmsub,
#ifdef RISCV_EXT_VECTOR
		[RV32V_BC_VLE32]   = &&rv32v_vle32,
		[RV32V_BC_VSE32]   = &&rv32v_vse32,
		[RV32V_BC_OP]      = &&rv32v_op,
#endif
		[RV32I_BC_NOP]     = &&rv32i_nop,
		[RV32A_BC_ATOMIC]  = &&rv32a_atomic,
		[RV32I_BC_INVALID] = &&rv32i_invalid,
	};
	static constexpr int OPCODES = sizeof(computed_opcode) / sizeof(computed_opcode[0]);

	// Decoded segments are always faster
	// So, always have at least the current segment
	if (UNLIKELY(m_exec == nullptr)) {
		this->next_execute_segment();
	}

	// Calculate the instruction limit
	if (imax != UINT64_MAX)
		machine().set_max_instructions(machine().instruction_counter() + imax);
	else
		machine().set_max_instructions(UINT64_MAX);

	InstrCounter counter{machine()};

	DecodedExecuteSegment<W>* exec;
	DecoderData<W>* exec_decoder;
	DecoderData<W>* decoder;
	address_t current_begin;
	address_t current_end;
	address_t pc;
	unsigned  count;
restart_sim:
	exec = this->m_exec;
	exec_decoder = exec->decoder_cache();
	current_begin = exec->exec_begin();
	current_end = exec->exec_end();

	pc = this->pc();

continue_segment:
	decoder = &exec_decoder[pc / DecoderCache<W>::DIVISOR];

continue_block:
	count = decoder->idxend;
	pc += count * 4;
	counter.increment_counter(count + 1);

if constexpr (EXTRA_CHECKS) {
	if (decoder->get_bytecode() < 0 || decoder->get_bytecode() >= OPCODES)
		throw MachineException(ILLEGAL_OPERATION, "Jumping to illegal opcode");
}
	goto *computed_opcode[decoder->get_bytecode()];

rv32i_li: {
	VIEW_INSTR();
	this->reg(instr.Itype.rd) = instr.Itype.signed_imm();
	NEXT_INSTR();
}
rv32i_addi: {
	if constexpr (decoder_rewriter_enabled) {
		VIEW_INSTR_AS(fi, FasterItype);
		this->reg(fi.rs1) =
			this->reg(fi.rs2) + fi.signed_imm();
	} else {
		VIEW_INSTR();
		this->reg(instr.Itype.rd) =
			this->reg(instr.Itype.rs1) + instr.Itype.signed_imm();
	}
	NEXT_INSTR();
}
rv32i_lui: {
	VIEW_INSTR();
	this->reg(instr.Utype.rd) = instr.Utype.upper_imm();
	NEXT_INSTR();
}
rv32i_auipc: {
	VIEW_INSTR();
	this->reg(instr.Utype.rd) = pc + instr.Utype.upper_imm();
	NEXT_BLOCK(4);
}
rv32i_ldw: {
	VIEW_INSTR();
	const auto addr = this->reg(instr.Itype.rs1) + instr.Itype.signed_imm();
	this->reg(instr.Itype.rd) =
		(int32_t)machine().memory.template read<uint32_t>(addr);
	NEXT_INSTR();
}
rv32i_sdw: {
	VIEW_INSTR();
	const auto addr  = reg(instr.Stype.rs1) + instr.Stype.signed_imm();
	machine().memory.template write<uint32_t>(addr, reg(instr.Stype.rs2));
	NEXT_INSTR();
}
rv32i_beq: {
	VIEW_INSTR();
	if (reg(instr.Btype.rs1) == reg(instr.Btype.rs2)) {
		PERFORM_BRANCH();
	}
	NEXT_BLOCK(4);
}
rv32i_bne: {
	VIEW_INSTR();
	if constexpr (decoder_rewriter_enabled) {
		VIEW_INSTR_AS(fi, FasterItype);
		if (reg(fi.rs1) != reg(fi.rs2)) {
			PERFORM_FAST_BRANCH();
		}
	} else {
		if (reg(instr.Btype.rs1) != reg(instr.Btype.rs2)) {
			PERFORM_BRANCH();
		}
	}
	NEXT_BLOCK(4);
}
rv32i_blt: {
	VIEW_INSTR();
	if ((saddr_t)reg(instr.Btype.rs1) < (saddr_t)reg(instr.Btype.rs2)) {
		PERFORM_BRANCH();
	}
	NEXT_BLOCK(4);
}
rv32i_bge: {
	VIEW_INSTR();
	if ((saddr_t)reg(instr.Btype.rs1) >= (saddr_t)reg(instr.Btype.rs2)) {
		PERFORM_BRANCH();
	}
	NEXT_BLOCK(4);
}
rv32i_bltu: {
	VIEW_INSTR();
	if (reg(instr.Btype.rs1) < reg(instr.Btype.rs2)) {
		PERFORM_BRANCH();
	}
	NEXT_BLOCK(4);
}
rv32i_bgeu: {
	VIEW_INSTR();
	if (reg(instr.Btype.rs1) >= reg(instr.Btype.rs2)) {
		PERFORM_BRANCH();
	}
	NEXT_BLOCK(4);
}
rv32i_op_add: {
	VIEW_INSTR();
	if constexpr (decoder_rewriter_enabled) {
		VIEW_INSTR_AS(fi, FasterOpType);
		this->reg(fi.rd) = reg(fi.rs1) + reg(fi.rs2);
	} else {
		this->reg(instr.Rtype.rd) =
			reg(instr.Rtype.rs1) + reg(instr.Rtype.rs2);
	}
	NEXT_INSTR();
}
rv32i_op_sub: {
	VIEW_INSTR();
	this->reg(instr.Rtype.rd) =
		reg(instr.Rtype.rs1) - reg(instr.Rtype.rs2);
	NEXT_INSTR();
}
rv32i_slli: {
	VIEW_INSTR();
	// SLLI: Logical left-shift 5/6/7-bit immediate
	this->reg(instr.Itype.rd) =
		reg(instr.Itype.rs1) << (instr.Itype.imm & (XLEN - 1));
	NEXT_INSTR();
}
rv32i_slti: {
	VIEW_INSTR();
	// SLTI: Set less than immediate
	this->reg(instr.Itype.rd) =
		(saddr_t(reg(instr.Itype.rs1)) < instr.Itype.signed_imm());
	NEXT_INSTR();
}
rv32i_sltiu: {
	VIEW_INSTR();
	// SLTIU: Sign-extend, then treat as unsigned
	this->reg(instr.Itype.rd) =
		(reg(instr.Itype.rs1) < addr_t(instr.Itype.signed_imm()));
	NEXT_INSTR();
}
rv32i_xori: {
	VIEW_INSTR();
	// XORI
	this->reg(instr.Itype.rd) =
		reg(instr.Itype.rs1) ^ instr.Itype.signed_imm();
	NEXT_INSTR();
}
rv32i_srli: {
	VIEW_INSTR();
	// SRLI: Shift-right logical 5/6/7-bit immediate
	this->reg(instr.Itype.rd) =
		reg(instr.Itype.rs1) >> (instr.Itype.imm & (XLEN - 1));
	NEXT_INSTR();
}
rv32i_srai: {
	VIEW_INSTR();
	// SRAI: Shift-right arithmetical (preserve the sign bit)
	this->reg(instr.Itype.rd) =
		saddr_t(reg(instr.Itype.rs1)) >> (instr.Itype.imm & (XLEN - 1));
	NEXT_INSTR();
}
rv32i_ori: {
	VIEW_INSTR();
	// ORI: Or sign-extended 12-bit immediate
	this->reg(instr.Itype.rd) =
		reg(instr.Itype.rs1) | instr.Itype.signed_imm();
	NEXT_INSTR();
}
rv32i_andi: {
	VIEW_INSTR();
	// ANDI: And sign-extended 12-bit immediate
	this->reg(instr.Itype.rd) =
		reg(instr.Itype.rs1) & instr.Itype.signed_imm();
	NEXT_INSTR();
}
rv32i_syscall: {
	// Make the current PC visible
	this->registers().pc = pc;
	// Make the instruction counter visible
	counter.apply();
	// Invoke system call
	machine().system_call(this->reg(REG_ECALL));
	if (UNLIKELY(counter.overflowed() || pc != this->registers().pc))
	{
		pc = registers().pc;
		goto check_jump;
	}
	NEXT_BLOCK(4);
}
rv32i_jal: {
	VIEW_INSTR();
	// Link *next* instruction (rd = PC + 4)
	if (instr.Jtype.rd != 0) {
		reg(instr.Jtype.rd) = pc + 4;
	}
	// And jump relative
	pc += instr.Jtype.jump_offset();
	goto check_jump;
}
rv32i_jalr: {
	VIEW_INSTR();
	// jump to register + immediate
	// NOTE: if rs1 == rd, avoid clobber by storing address first
	const auto address = reg(instr.Itype.rs1) + instr.Itype.signed_imm();
	// Link *next* instruction (rd = PC + 4)
	if (instr.Itype.rd != 0) {
		reg(instr.Itype.rd) = pc + 4;
	}
	pc = address;
	goto check_jump;
}

rv32i_op_sll: {
	VIEW_INSTR();
	#define OPREGS() \
		auto& dst = reg(instr.Rtype.rd); \
		const auto src1 = reg(instr.Rtype.rs1); \
		const auto src2 = reg(instr.Rtype.rs2);
	OPREGS();
	dst = src1 << (src2 & (XLEN-1));
	NEXT_INSTR();
}
rv32i_op_slt: {
	VIEW_INSTR();
	OPREGS();
	dst = (saddr_t(src1) < saddr_t(src2));
	NEXT_INSTR();
}
rv32i_op_sltu: {
	VIEW_INSTR();
	OPREGS();
	dst = (src1 < src2);
	NEXT_INSTR();
}
rv32i_op_xor: {
	VIEW_INSTR();
	OPREGS();
	dst = src1 ^ src2;
	NEXT_INSTR();
}
rv32i_op_srl: {
	VIEW_INSTR();
	OPREGS();
	dst = src1 >> (src2 & (XLEN - 1));
	NEXT_INSTR();
}
rv32i_op_or: {
	VIEW_INSTR();
	OPREGS();
	dst = src1 | src2;
	NEXT_INSTR();
}
rv32i_op_and: {
	VIEW_INSTR();
	OPREGS();
	dst = src1 & src2;
	NEXT_INSTR();
}
rv32i_op_mul: {
	VIEW_INSTR();
	OPREGS();
	dst = saddr_t(src1) * saddr_t(src2);
	NEXT_INSTR();
}
rv32i_op_mulh: {
	VIEW_INSTR();
	OPREGS();
	if constexpr (W == 4) {
		dst = uint64_t((int64_t)saddr_t(src1) * (int64_t)saddr_t(src2)) >> 32u;
	} else if constexpr (W == 8) {
		dst = ((__int128_t) src1 * (__int128_t) src2) >> 64u;
	} else {
		dst = 0;
	}
	NEXT_INSTR();
}
rv32i_op_mulhsu: {
	VIEW_INSTR();
	OPREGS();
	if constexpr (W == 4) {
		dst = uint64_t((int64_t)saddr_t(src1) * (uint64_t)src2) >> 32u;
	} else if constexpr (W == 8) {
		dst = ((__int128_t) src1 * (__int128_t) src2) >> 64u;
	} else {
		dst = 0;
	}
	NEXT_INSTR();
}
rv32i_op_mulhu: {
	VIEW_INSTR();
	OPREGS();
	if constexpr (W == 4) {
		dst = uint64_t((uint64_t)src1 * (uint64_t)src2) >> 32u;
	} else if constexpr (W == 8) {
		dst = ((__int128_t) src1 * (__int128_t) src2) >> 64u;
	} else {
		dst = 0;
	}
	NEXT_INSTR();
}
rv32i_op_div: {
	VIEW_INSTR();
	OPREGS();
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
rv32i_op_divu: {
	VIEW_INSTR();
	OPREGS();
	if (LIKELY(src2 != 0)) {
		dst = src1 / src2;
	} else {
		dst = addr_t(-1);
	}
	NEXT_INSTR();
}
rv32i_op_rem: {
	VIEW_INSTR();
	OPREGS();
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
rv32i_op_remu: {
	VIEW_INSTR();
	OPREGS();
	if (LIKELY(src2 != 0)) {
		dst = src1 % src2;
	} else {
		dst = addr_t(-1);
	}
	NEXT_INSTR();
}
rv32i_op_sra: {
	VIEW_INSTR();
	OPREGS();
	dst = saddr_t(src1) >> (src2 & (XLEN-1));
	NEXT_INSTR();
}
rv32i_op_sh1add: {
	VIEW_INSTR();
	OPREGS();
	dst = src2 + (src1 << 1);
	NEXT_INSTR();
}
rv32i_op_sh2add: {
	VIEW_INSTR();
	OPREGS();
	dst = src2 + (src1 << 2);
	NEXT_INSTR();
}
rv32i_op_sh3add: {
	VIEW_INSTR();
	OPREGS();
	dst = src2 + (src1 << 3);
	NEXT_INSTR();
}

rv32i_ldb: {
	VIEW_INSTR();
	const auto addr = reg(instr.Itype.rs1) + instr.Itype.signed_imm();
	reg(instr.Itype.rd) =
		int8_t(machine().memory.template read<uint8_t>(addr));
	NEXT_INSTR();
}
rv32i_ldbu: {
	VIEW_INSTR();
	const auto addr = reg(instr.Itype.rs1) + instr.Itype.signed_imm();
	reg(instr.Itype.rd) =
		saddr_t(machine().memory.template read<uint8_t>(addr));
	NEXT_INSTR();
}
rv32i_ldh: {
	VIEW_INSTR();
	const auto addr = reg(instr.Itype.rs1) + instr.Itype.signed_imm();
	reg(instr.Itype.rd) =
		int16_t(machine().memory.template read<uint16_t>(addr));
	NEXT_INSTR();
}
rv32i_ldhu: {
	VIEW_INSTR();
	const auto addr = reg(instr.Itype.rs1) + instr.Itype.signed_imm();
	reg(instr.Itype.rd) =
		saddr_t(machine().memory.template read<uint16_t>(addr));
	NEXT_INSTR();
}
rv32i_sdb: {
	VIEW_INSTR();
	const auto addr = reg(instr.Stype.rs1) + instr.Stype.signed_imm();
	machine().memory.template write<uint8_t>(addr, reg(instr.Stype.rs2));
	NEXT_INSTR();
}
rv32i_sdh: {
	VIEW_INSTR();
	const auto addr = reg(instr.Stype.rs1) + instr.Stype.signed_imm();
	machine().memory.template write<uint16_t>(addr, reg(instr.Stype.rs2));
	NEXT_INSTR();
}

rv32i_flw: {
	VIEW_INSTR_AS(fi, rv32f_instruction);
	auto addr = reg(fi.Itype.rs1) + fi.Itype.signed_imm();
	auto& dst = registers().getfl(fi.Itype.rd);
	dst.load_u32(machine().memory.template read<uint32_t> (addr));
	NEXT_INSTR();
}
rv32i_fld: {
	VIEW_INSTR_AS(fi, rv32f_instruction);
	auto addr = reg(fi.Itype.rs1) + fi.Itype.signed_imm();
	auto& dst = registers().getfl(fi.Itype.rd);
	dst.load_u64(machine().memory.template read<uint64_t> (addr));
	NEXT_INSTR();
}
rv32i_fsw: {
	VIEW_INSTR_AS(fi, rv32f_instruction);
	const auto& src = registers().getfl(fi.Stype.rs2);
	auto addr = reg(fi.Stype.rs1) + fi.Stype.signed_imm();
	machine().memory.template write<uint32_t> (addr, src.i32[0]);
	NEXT_INSTR();
}
rv32i_fsd: {
	VIEW_INSTR_AS(fi, rv32f_instruction);
	const auto& src = registers().getfl(fi.Stype.rs2);
	auto addr = reg(fi.Stype.rs1) + fi.Stype.signed_imm();
	machine().memory.template write<uint64_t> (addr, src.i64);
	NEXT_INSTR();
}
rv32i_nop: {
	NEXT_INSTR();
}
rv32i_system: {
	VIEW_INSTR();
	machine().system(instr);
	// Check if machine stopped
	if (UNLIKELY(counter.overflowed()))
	{
		pc = registers().pc;
		return;
	}
	NEXT_BLOCK(4);
}
rv32f_fpfunc: {
	VIEW_INSTR();
	const rv32f_instruction fi{instr};
	auto& dst = registers().getfl(fi.R4type.rd);
	auto& rs1 = registers().getfl(fi.R4type.rs1);
	auto& rs2 = registers().getfl(fi.R4type.rs2);
	// TODO: Split this up into handlers
	switch (instr.fpfunc())
	{
		case 0b00000: {
			if (fi.R4type.funct2 == 0x0)
			{ // float32
				dst.set_float(rs1.f32[0] + rs2.f32[0]);
			}
			else if (fi.R4type.funct2 == 0x1)
			{ // float64
				dst.f64 = rs1.f64 + rs2.f64;
			}
			else {
				trigger_exception(ILLEGAL_OPERATION);
			}
			break;
		}
		case 0b00001:
			if (fi.R4type.funct2 == 0x0)
			{ // float32
				dst.set_float(rs1.f32[0] - rs2.f32[0]);
			}
			else if (fi.R4type.funct2 == 0x1)
			{ // float64
				dst.f64 = rs1.f64 - rs2.f64;
			}
			else {
				trigger_exception(ILLEGAL_OPERATION);
			}
			break;
		case 0b00010:
			if (fi.R4type.funct2 == 0x0)
			{ // float32
				dst.set_float(rs1.f32[0] * rs2.f32[0]);
			}
			else if (fi.R4type.funct2 == 0x1)
			{ // float64
				dst.f64 = rs1.f64 * rs2.f64;
			}
			else {
				trigger_exception(ILLEGAL_OPERATION);
			}
			break;
		case 0b00011:
			INVOKE_INSTR(FDIV);
			break;
		case 0b00100:
			INVOKE_INSTR(FSGNJ_NX);
			break;
		case 0b00101:
			INVOKE_INSTR(FMIN_FMAX);
			break;
		case 0b01011:
			INVOKE_INSTR(FSQRT);
			break;
		case 0b10100:
			if (rv32f_instruction { instr }.R4type.rd != 0) {
				INVOKE_INSTR(FEQ_FLT_FLE);
			}
			break;
		case 0b01000:
			INVOKE_INSTR(FCVT_SD_DS);
			break;
		case 0b11000:
			if (rv32f_instruction { instr }.R4type.rd != 0) {
				INVOKE_INSTR(FCVT_W_SD);
			}
			break;
		case 0b11010:
			INVOKE_INSTR(FCVT_SD_W);
			break;
		case 0b11100:
			if (rv32f_instruction { instr }.R4type.rd != 0) {
				if (rv32f_instruction { instr }.R4type.funct3 == 0) {
					INVOKE_INSTR(FMV_X_W);
				} else {
					INVOKE_INSTR(FCLASS);
				}
			}
			break;
		case 0b11110:
			INVOKE_INSTR(FMV_W_X);
			break;
		default:
			this->execute(instr);
	}
	NEXT_INSTR();
}
rv32f_fmadd: {
	VIEW_INSTR();
	INVOKE_INSTR(FMADD);
	NEXT_INSTR();
}
rv32f_fmsub: {
	VIEW_INSTR();
	INVOKE_INSTR(FMSUB);
	NEXT_INSTR();
}
rv32f_fnmadd: {
	VIEW_INSTR();
	INVOKE_INSTR(FNMADD);
	NEXT_INSTR();
}
rv32f_fnmsub: {
	VIEW_INSTR();
	INVOKE_INSTR(FNMSUB);
	NEXT_INSTR();
}
#ifdef RISCV_EXT_VECTOR
rv32v_vle32: {
	VIEW_INSTR();
	INVOKE_INSTR(VLE32);
	NEXT_INSTR();
}
rv32v_vse32: {
	VIEW_INSTR();
	INVOKE_INSTR(VSE32);
	NEXT_INSTR();
}
rv32v_op: {
	VIEW_INSTR();
	switch (instr.vwidth()) {
	case 0x0: // OPI.VV
		INVOKE_INSTR(VOPI_VV);
		break;
	case 0x1: // OPF.VV
		INVOKE_INSTR(VOPF_VV);
		break;
	case 0x2: // OPM.VV
		INVOKE_INSTR(VOPM_VV);
		break;
	case 0x3: // OPI.VI
		INVOKE_INSTR(VOPI_VI);
		break;
	case 0x5: // OPF.VF
		INVOKE_INSTR(VOPF_VF);
		break;
	case 0x7: // Vector Configuration
		switch (instr.vsetfunc()) {
		case 0x0:
		case 0x1:
			INVOKE_INSTR(VSETVLI);
			break;
		case 0x2:
			INVOKE_INSTR(VSETVL);
			break;
		case 0x3:
			INVOKE_INSTR(VSETIVLI);
			break;
		}
		break;
	}
	NEXT_INSTR();
}
#endif // RISCV_EXT_VECTOR
rv32i_full_decode: {
	VIEW_INSTR();
	this->execute(instr);
	NEXT_INSTR();
}
rv32a_atomic: {
	VIEW_INSTR();
	this->execute(instr);
	NEXT_INSTR();
}
rv32i_invalid:
	this->trigger_exception(
		UNIMPLEMENTED_INSTRUCTION, decoder->instr);

check_jump:
	if (UNLIKELY(counter.overflowed())) {
		registers().pc = pc;
		return;
	}
	if (UNLIKELY(!(pc >= current_begin && pc < current_end)))
	{
		registers().pc = pc;
		this->next_execute_segment();
		goto restart_sim;
	}
	goto continue_segment;

} // CPU::simulate_computed()

template <int W>
size_t CPU<W>::computed_index_for(rv32i_instruction instr)
{
	switch (instr.opcode())
	{
		case RV32I_LOAD:
			// XXX: Support dummy loads
			if (instr.Itype.rd == 0)
				return RV32I_BC_NOP;
			switch (instr.Itype.funct3) {
			case 0x0: // LD.B
				return RV32I_BC_LDB;
			case 0x1: // LD.H
				return RV32I_BC_LDH;
			case 0x2: // LD.W
				return RV32I_BC_LDW;
			case 0x3:
				if constexpr (W >= 8) {
					return RV32I_BC_LDD;
				}
				return RV32I_BC_INVALID;
			case 0x4: // LD.BU
				return RV32I_BC_LDBU;
			case 0x5: // LD.HU
				return RV32I_BC_LDHU;
			default:
				return RV32I_BC_INVALID;
			}
		case RV32I_STORE:
			switch (instr.Stype.funct3)
			{
			case 0x0: // SD.B
				return RV32I_BC_SDB;
			case 0x1: // SD.H
				return RV32I_BC_SDH;
			case 0x2: // SD.W
				return RV32I_BC_SDW;
			case 0x3:
				if constexpr (W >= 8) {
					return RV32I_BC_SDD;
				}
				return RV32I_BC_INVALID;
			default:
				return RV32I_BC_INVALID;
			}
		case RV32I_BRANCH:
			switch (instr.Btype.funct3) {
			case 0x0: // BEQ
				return RV32I_BC_BEQ;
			case 0x1: // BNE
				return RV32I_BC_BNE;
			case 0x4: // BLT
				return RV32I_BC_BLT;
			case 0x5: // BGE
				return RV32I_BC_BGE;
			case 0x6: // BLTU
				return RV32I_BC_BLTU;
			case 0x7: // BGEU
				return RV32I_BC_BGEU;
			default:
				return RV32I_BC_INVALID;
			}
		case RV32I_LUI:
			if (instr.Utype.rd == 0)
				return RV32I_BC_NOP;
			return RV32I_BC_LUI;
		case RV32I_AUIPC:
			if (instr.Utype.rd == 0)
				return RV32I_BC_NOP;
			return RV32I_BC_AUIPC;
		case RV32I_JAL:
			return RV32I_BC_JAL;
		case RV32I_JALR:
			return RV32I_BC_JALR;
		case RV32I_OP_IMM:
			if (instr.Itype.rd == 0)
				return RV32I_BC_NOP;
			switch (instr.Itype.funct3)
			{
			case 0x0:
				if (instr.Itype.rs1 == 0)
					return RV32I_BC_LI;
				else
					return RV32I_BC_ADDI;
			case 0x1: // SLLI
				return RV32I_BC_SLLI;
			case 0x2: // SLTI
				return RV32I_BC_SLTI;
			case 0x3: // SLTIU
				return RV32I_BC_SLTIU;
			case 0x4: // XORI
				return RV32I_BC_XORI;
			case 0x5:
				if (instr.Itype.is_srai())
					return RV32I_BC_SRAI;
				else
					return RV32I_BC_SRLI;
			case 0x6:
				return RV32I_BC_ORI;
			case 0x7:
				return RV32I_BC_ANDI;
			default:
				return RV32I_BC_INVALID;
			}
		case RV32I_OP:
			if (instr.Itype.rd == 0)
				return RV32I_BC_NOP;
			switch (instr.Rtype.jumptable_friendly_op())
			{
			case 0x0:
				return RV32I_BC_OP_ADD;
			case 0x200:
				return RV32I_BC_OP_SUB;
			case 0x1:
				return RV32I_BC_OP_SLL;
			case 0x2:
				return RV32I_BC_OP_SLT;
			case 0x3:
				return RV32I_BC_OP_SLTU;
			case 0x4:
				return RV32I_BC_OP_XOR;
			case 0x5:
				return RV32I_BC_OP_SRL;
			case 0x6:
				return RV32I_BC_OP_OR;
			case 0x7:
				return RV32I_BC_OP_AND;
			case 0x10:
				return RV32I_BC_OP_MUL;
			case 0x11:
				return RV32I_BC_OP_MULH;
			case 0x12:
				return RV32I_BC_OP_MULHSU;
			case 0x13:
				return RV32I_BC_OP_MULHU;
			case 0x14:
				return RV32I_BC_OP_DIV;
			case 0x15:
				return RV32I_BC_OP_DIVU;
			case 0x16:
				return RV32I_BC_OP_REM;
			case 0x17:
				return RV32I_BC_OP_REMU;
			case 0x102:
				return RV32I_BC_OP_SH1ADD;
			case 0x104:
				return RV32I_BC_OP_SH2ADD;
			case 0x106:
				return RV32I_BC_OP_SH3ADD;
			//case 0x204:
			//	return RV32I_BC_OP_XNOR;
			case 0x205:
				return RV32I_BC_OP_SRA;
			default:
				return RV32I_BC_INVALID;
			}
		case RV32I_SYSTEM:
			if (LIKELY(instr.Itype.funct3 == 0))
			{
				if (instr.Itype.imm == 0) {
					return RV32I_BC_SYSCALL;
				}
			}
			return RV32I_BC_SYSTEM;
		case RV32I_FENCE:
			return RV32I_BC_NOP;
		case RV32F_LOAD: {
			const rv32f_instruction fi{instr};
			switch (fi.Itype.funct3) {
			case 0x2: // FLW
				return RV32F_BC_FLW;
			case 0x3: // FLD
				return RV32F_BC_FLD;
#ifdef RISCV_EXT_VECTOR
			case 0x6: // VLE32
				return RV32V_BC_VLE32;
#endif
			default:
				return RV32I_BC_INVALID;
			}
		}
		case RV32F_STORE: {
			const rv32f_instruction fi{instr};
			switch (fi.Itype.funct3) {
			case 0x2: // FSW
				return RV32F_BC_FSW;
			case 0x3: // FSD
				return RV32F_BC_FSD;
#ifdef RISCV_EXT_VECTOR
			case 0x6: // VSE32
				return RV32V_BC_VSE32;
#endif
			default:
				return RV32I_BC_INVALID;
			}
		}
		case RV32F_FMADD:
			return RV32F_BC_FMADD;
		case RV32F_FMSUB:
			return RV32F_BC_FMSUB;
		case RV32F_FNMADD:
			return RV32F_BC_FNMADD;
		case RV32F_FNMSUB:
			return RV32F_BC_FNMSUB;
		case RV32F_FPFUNC:
			return RV32F_BC_FPFUNC;
#ifdef RISCV_EXT_VECTOR
		case RV32V_OP:
			return RV32V_BC_OP;
#endif
		case RV32A_ATOMIC:
			return RV32A_BC_ATOMIC;
		default:
			return RV32I_BC_INVALID;
	}
} // computed_index_for()

	template struct CPU<4>;
	template struct CPU<8>;
	template struct CPU<16>;
} // riscv
