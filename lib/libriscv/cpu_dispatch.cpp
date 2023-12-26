#include "machine.hpp"
#include "decoder_cache.hpp"
#include "instruction_counter.hpp"
#include "threaded_bytecodes.hpp"
#include "rv32i_instr.hpp"
#include "rvfd.hpp"
#ifdef RISCV_EXT_COMPRESSED
#include "rvc.hpp"
#endif
#ifdef RISCV_EXT_VECTOR
#include "rvv.hpp"
#endif

/**
 * This file is included by threaded_dispatch.cpp and bytecode_dispatch.cpp
 * It implements the logic for switch-based and threaded dispatch.
 * 
 * All dispatch modes share bytecode_impl.cpp
**/

namespace riscv
{
	static constexpr bool VERBOSE_JUMPS = riscv::verbose_branches_enabled;
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
	static constexpr bool FUZZING = true;
#else
	static constexpr bool FUZZING = false;
#endif

#define VIEW_INSTR() \
	auto instr = *(rv32i_instruction *)&decoder->instr;
#define VIEW_INSTR_AS(name, x) \
	auto &&name = *(x *)&decoder->instr;
#define NEXT_INSTR()                  \
	if constexpr (compressed_enabled) \
		decoder += 2;                 \
	else                              \
		decoder += 1;                 \
	EXECUTE_INSTR();
#define NEXT_C_INSTR() \
	decoder += 1;      \
	EXECUTE_INSTR();

#define NEXT_BLOCK(len, OF)               \
	pc += len;                            \
	decoder += len / DecoderCache<W>::DIVISOR;               \
	if constexpr (OF) {						\
		if (UNLIKELY(counter.overflowed())) \
			goto check_jump;				\
	}										\
	if constexpr (FUZZING) /* Give OOB-aid to ASAN */        \
	decoder = &exec_decoder[pc / DecoderCache<W>::DIVISOR];  \
	pc += decoder->block_bytes();                            \
	counter.increment_counter(decoder->instruction_count()); \
	EXECUTE_INSTR();

#define NEXT_SEGMENT()                                       \
	decoder = &exec_decoder[pc / DecoderCache<W>::DIVISOR];  \
	pc += decoder->block_bytes();                            \
	counter.increment_counter(decoder->instruction_count()); \
	EXECUTE_INSTR();

#define PERFORM_BRANCH()                 \
	if constexpr (VERBOSE_JUMPS) fprintf(stderr, "Branch 0x%lX >= 0x%lX (decoder=%p)\n", long(pc), long(pc + fi.signed_imm()), decoder); \
	if (LIKELY(!counter.overflowed())) { \
		NEXT_BLOCK(fi.signed_imm(), false);     \
	}                                    \
	pc += fi.signed_imm();               \
	goto check_jump;

#define PERFORM_FORWARD_BRANCH()         \
	if constexpr (VERBOSE_JUMPS) fprintf(stderr, "Fw.Branch 0x%lX >= 0x%lX\n", long(pc), long(pc + fi.signed_imm())); \
	NEXT_BLOCK(fi.signed_imm(), false);

#define OVERFLOW_CHECKED_JUMP() \
	goto check_jump


template <int W> DISPATCH_ATTR
bool CPU<W>::simulate(uint64_t inscounter, uint64_t maxcounter)
{
	static constexpr uint32_t XLEN = W * 8;
	using addr_t  = address_type<W>;
	using saddr_t = signed_address_type<W>;

#ifdef DISPATCH_MODE_THREADED
	static constexpr void *computed_opcode[] = {
		[RV32I_BC_INVALID] = &&execute_invalid,
		[RV32I_BC_ADDI]    = &&rv32i_addi,
		[RV32I_BC_LI]      = &&rv32i_li,
		[RV32I_BC_MV]      = &&rv32i_mv,
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
		[RV32I_BC_BEQ_FW]  = &&rv32i_beq_fw,
		[RV32I_BC_BNE_FW]  = &&rv32i_bne_fw,

		[RV32I_BC_JAL]     = &&rv32i_jal,
		[RV32I_BC_JALR]    = &&rv32i_jalr,
		[RV32I_BC_FAST_JAL] = &&rv32i_fast_jal,
		[RV32I_BC_FAST_CALL] = &&rv32i_fast_call,

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
		[RV32I_BC_OP_DIV]  = &&rv32i_op_div,
		[RV32I_BC_OP_DIVU] = &&rv32i_op_divu,
		[RV32I_BC_OP_REM]  = &&rv32i_op_rem,
		[RV32I_BC_OP_REMU] = &&rv32i_op_remu,
		[RV32I_BC_OP_SRA]  = &&rv32i_op_sra,
		[RV32I_BC_OP_ADD_UW] = &&rv32i_op_add_uw,
		[RV32I_BC_OP_ZEXT_H] = &&rv32i_op_zext_h,
		[RV32I_BC_OP_SH1ADD] = &&rv32i_op_sh1add,
		[RV32I_BC_OP_SH2ADD] = &&rv32i_op_sh2add,
		[RV32I_BC_OP_SH3ADD] = &&rv32i_op_sh3add,

		[RV32I_BC_SEXT_B] = &&rv32i_sext_b,
		[RV32I_BC_SEXT_H] = &&rv32i_sext_h,
		[RV32I_BC_BSETI]  = &&rv32i_bseti,
		[RV32I_BC_BEXTI]  = &&rv32i_bexti,

#ifdef RISCV_64I
		[RV64I_BC_ADDIW]  = &&rv64i_addiw,
		[RV64I_BC_SRLIW]  = &&rv64i_srliw,
		[RV64I_BC_SRAIW]  = &&rv64i_sraiw,
		[RV64I_BC_OP_ADDW] = &&rv64i_op_addw,
		[RV64I_BC_OP_SH1ADD_UW] = &&rv64i_op_sh1add_uw,
		[RV64I_BC_OP_SH2ADD_UW] = &&rv64i_op_sh2add_uw,
#endif // RISCV_64I

#ifdef RISCV_EXT_COMPRESSED
		[RV32C_BC_ADDI]     = &&rv32c_addi,
		[RV32C_BC_LI]       = &&rv32c_addi,
		[RV32C_BC_MV]       = &&rv32c_mv,
		[RV32C_BC_SLLI]     = &&rv32c_slli,
		[RV32C_BC_BEQZ]     = &&rv32c_beqz,
		[RV32C_BC_BNEZ]     = &&rv32c_bnez,
		[RV32C_BC_JMP]      = &&rv32c_jmp,
		[RV32C_BC_JR]       = &&rv32c_jr,
		[RV32C_BC_JAL_ADDIW]= &&rv32c_jal_addiw,
		[RV32C_BC_JALR]     = &&rv32c_jalr,
		[RV32C_BC_LDD]      = &&rv32c_ldd,
		[RV32C_BC_STD]      = &&rv32c_std,
		[RV32C_BC_FUNCTION] = &&rv32c_func,
		[RV32C_BC_JUMPFUNC] = &&rv32c_jumpfunc,
#endif

		[RV32I_BC_SYSCALL] = &&rv32i_syscall,
		[RV32I_BC_STOP]    = &&rv32i_stop,
		[RV32I_BC_NOP]     = &&rv32i_nop,

		[RV32F_BC_FLW]     = &&rv32i_flw,
		[RV32F_BC_FLD]     = &&rv32i_fld,
		[RV32F_BC_FSW]     = &&rv32i_fsw,
		[RV32F_BC_FSD]     = &&rv32i_fsd,
		[RV32F_BC_FADD]    = &&rv32f_fadd,
		[RV32F_BC_FSUB]    = &&rv32f_fsub,
		[RV32F_BC_FMUL]    = &&rv32f_fmul,
		[RV32F_BC_FDIV]    = &&rv32f_fdiv,
		[RV32F_BC_FMADD]   = &&rv32f_fmadd,
#ifdef RISCV_EXT_VECTOR
		[RV32V_BC_VLE32]   = &&rv32v_vle32,
		[RV32V_BC_VSE32]   = &&rv32v_vse32,
		[RV32V_BC_VFADD_VV] = &&rv32v_vfadd_vv,
		[RV32V_BC_VFMUL_VF] = &&rv32v_vfmul_vf,
#endif
		[RV32I_BC_FUNCTION] = &&execute_decoded_function,
		[RV32I_BC_FUNCBLOCK] = &&execute_function_block,
		[RV32I_BC_TRANSLATOR] = &&translated_function,
		[RV32I_BC_SYSTEM]  = &&rv32i_system,
	};
#endif

	InstrCounter counter{machine(), inscounter, maxcounter};

	DecodedExecuteSegment<W>* exec = this->m_exec;
	address_t current_begin = exec->exec_begin();
	address_t current_end = exec->exec_end();
	address_t pc = this->pc();

	DecoderData<W>* exec_decoder = exec->decoder_cache();
	DecoderData<W>* decoder;

	// We need an execute segment matching current PC
	if (UNLIKELY(!(pc >= current_begin && pc < current_end)))
		goto new_execute_segment;

continue_segment:
	decoder = &exec_decoder[pc / DecoderCache<W>::DIVISOR];
	pc += decoder->block_bytes();
	counter.increment_counter(decoder->instruction_count());

#ifdef DISPATCH_MODE_SWITCH_BASED

while (true) {
	switch (decoder->get_bytecode()) {
	#define INSTRUCTION(bc, lbl) case bc:

#else

	goto *computed_opcode[decoder->get_bytecode()];
	#define INSTRUCTION(bc, lbl) lbl:

#endif

#define DECODER()   (*decoder)
#define CPU()       (*this)
#define REG(x)      registers().get()[x]
#define REGISTERS() registers()
#define VECTORS()   registers().rvv()
#define MACHINE()   machine()

	/** Instruction handlers **/

#define BYTECODES_OP_IMM
#  include "bytecode_impl.cpp"
#undef BYTECODES_OP_IMM

#define BYTECODES_LOAD_STORE
#  include "bytecode_impl.cpp"
#undef BYTECODES_LOAD_STORE

#define BYTECODES_BRANCH
#  include "bytecode_impl.cpp"
#undef BYTECODES_BRANCH

INSTRUCTION(RV32I_BC_FAST_JAL, rv32i_fast_jal) {
	VIEW_INSTR();
	if constexpr (VERBOSE_JUMPS) {
		fprintf(stderr, "FAST_JAL PC 0x%lX => 0x%lX\n", long(pc), long(pc + instr.whole));
	}
	NEXT_BLOCK((int32_t)instr.whole, true);
}
INSTRUCTION(RV32I_BC_FAST_CALL, rv32i_fast_call) {
	VIEW_INSTR();
	if constexpr (VERBOSE_JUMPS) {
		fprintf(stderr, "FAST_CALL PC 0x%lX => 0x%lX\n", long(pc), long(pc + instr.whole));
	}
	reg(REG_RA) = pc + 4;
	NEXT_BLOCK((int32_t)instr.whole, true);
}

#define BYTECODES_OP
#  include "bytecode_impl.cpp"
#undef BYTECODES_OP

INSTRUCTION(RV32I_BC_SYSCALL, rv32i_syscall) {
	// Make the current PC visible
	this->registers().pc = pc;
	// Make the instruction counter(s) visible
	counter.apply();
	// Invoke system call
	machine().system_call(this->reg(REG_ECALL));
	// Restore max counter
	counter.retrieve_max_counter();
	if (UNLIKELY(counter.overflowed() || pc != this->registers().pc))
	{
		// System calls are always full-length instructions
		if constexpr (VERBOSE_JUMPS) {
			if (pc != this->registers().pc)
			fprintf(stderr, "SYSCALL jump from 0x%lX to 0x%lX\n",
				long(pc), long(registers().pc + 4));
		}
		pc = registers().pc + 4;
		goto check_jump;
	}
	NEXT_BLOCK(4, false);
}

#define BYTECODES_FLP
#  include "bytecode_impl.cpp"
#undef BYTECODES_FLP

/** UNLIKELY INSTRUCTIONS **/
/** UNLIKELY INSTRUCTIONS **/

INSTRUCTION(RV32I_BC_FUNCTION, execute_decoded_function) {
	VIEW_INSTR();
	//printf("Slowpath: 0x%lX\n", pc);
	auto handler = decoder->get_handler();
	handler(*this, instr);
	NEXT_INSTR();
}
INSTRUCTION(RV32I_BC_STOP, rv32i_stop) {
	REGISTERS().pc = pc + 4;
	return true;
}

INSTRUCTION(RV32I_BC_JAL, rv32i_jal) {
	VIEW_INSTR_AS(fi, FasterJtype);
	if (fi.rd != 0)
		reg(fi.rd) = pc + 4;
	if constexpr (VERBOSE_JUMPS) {
		fprintf(stderr, "JAL PC 0x%lX => 0x%lX\n", long(pc), long(pc+fi.offset));
	}
	pc += fi.offset;
	goto check_jump;
}

/** UNLIKELY INSTRUCTIONS **/
/** UNLIKELY INSTRUCTIONS **/

INSTRUCTION(RV32I_BC_TRANSLATOR, translated_function) {
#ifdef RISCV_BINARY_TRANSLATION
	VIEW_INSTR();
	// Invoke translated code
	auto bintr_results = 
		exec->unchecked_mapping_at(instr.whole)(*this, counter.value()-1, counter.max(), pc);
	counter.set_counters(bintr_results.counter, bintr_results.max_counter);
	// Translations always execute at least a block
	pc = registers().pc;
	goto check_jump;
#else
	trigger_exception(FEATURE_DISABLED, pc);
#endif
}

INSTRUCTION(RV32I_BC_SYSTEM, rv32i_system) {
	VIEW_INSTR();
	// Make the current PC visible
	this->registers().pc = pc;
	// Make the instruction counters visible
	counter.apply();
	// Invoke SYSTEM
	machine().system(instr);
	// Restore PC in case it changed (supervisor)
	pc = registers().pc + 4;
	goto check_jump;
}

INSTRUCTION(RV32I_BC_FUNCBLOCK, execute_function_block) {
	VIEW_INSTR();
	auto handler = decoder->get_handler();
	handler(*this, instr);
	NEXT_BLOCK(instr.length(), true);
}

#define BYTECODES_RARELY_USED
#  include "bytecode_impl.cpp"
#undef BYTECODES_RARELY_USED

#ifdef DISPATCH_MODE_SWITCH_BASED
	default:
		goto execute_invalid;
	} // switch case
} // while loop

#endif

check_jump:
	if (UNLIKELY(counter.overflowed()))
		goto counter_overflow;

	if (LIKELY(pc - current_begin < current_end - current_begin)) {
		goto continue_segment;
	}

	//if (UNLIKELY(!(pc >= current_begin && pc < current_end)))
	// We have to store and restore PC here as there are
	// custom callbacks when changing segments that can
	// jump around.
	registers().pc = pc;

new_execute_segment:
	// Change to a new execute segment
	exec = &this->next_execute_segment();
	exec_decoder = exec->decoder_cache();
	current_begin = exec->exec_begin();
	current_end = exec->exec_end();
	pc = registers().pc;
	goto continue_segment;

execute_invalid:
	trigger_exception(ILLEGAL_OPCODE, decoder->instr);

counter_overflow:
	registers().pc = pc;

	// Machine stopped normally?
	return counter.max() == 0;

} // CPU::simulate_XXX()

} // riscv
