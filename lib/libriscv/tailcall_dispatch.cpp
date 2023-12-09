#include "machine.hpp"
#include "decoder_cache.hpp"
#include "threaded_bytecodes.hpp"
#include "rv32i_instr.hpp"
#include "rvfd.hpp"
#ifdef RISCV_EXT_COMPRESSED
#include "rvc.hpp"
#endif
#ifdef RISCV_EXT_VECTOR
#include "rvv.hpp"
#endif

#define MUSTTAIL __attribute__((musttail))
#define MUNUSED  [[maybe_unused]]
#define DISPATCH_MODE_TAILCALL
#define INSTRUCTION(bytecode, name) \
	template <int W>                \
	static TcoRet<W> name(DecoderData<W>* d, MUNUSED DecodedExecuteSegment<W>* exec, MUNUSED CPU<W>& cpu, MUNUSED address_type<W> pc, MUNUSED uint64_t counter)
#define addr_t  address_type<W>
#define saddr_t signed_address_type<W>
#define XLEN    (8 * W)

#define VIEW_INSTR() \
	auto instr = *(rv32i_instruction *)&d->instr;
#define VIEW_INSTR_AS(name, x) \
	auto&& name = *(x *)&d->instr;
#define EXECUTE_INSTR() \
	computed_opcode<W>[d->get_bytecode()](d, exec, cpu, pc, counter)
#define EXECUTE_CURRENT()              \
	MUSTTAIL return EXECUTE_INSTR();
#define NEXT_INSTR()                   \
	d += (compressed_enabled ? 2 : 1); \
	EXECUTE_CURRENT()
#define NEXT_C_INSTR() \
	d += 1;            \
	EXECUTE_CURRENT()

#define COUNTER_OVERFLOWED(counter) \
	(counter >= cpu.machine().max_instructions())
#define RETURN_VALUES() \
	{pc, counter}
#define UNUSED_FUNCTION() \
	cpu.trigger_exception(ILLEGAL_OPCODE);

#define BEGIN_BLOCK()                               \
	pc += d->block_bytes(); \
	counter += d->instruction_count();
#define NEXT_BLOCK(len, OF)              \
	pc += len;                           \
	d += len / DecoderCache<W>::DIVISOR; \
	if constexpr (OF) {                  \
		OVERFLOW_CHECK();                \
	}									 \
	if constexpr (FUZZING) /* Give OOB-aid to ASAN */           \
	d = &exec->decoder_cache()[pc / DecoderCache<W>::DIVISOR];  \
	BEGIN_BLOCK()                        \
	EXECUTE_CURRENT()

#define QUICK_EXEC_CHECK()                                              \
	if (UNLIKELY(!(pc >= exec->exec_begin() && pc < exec->exec_end()))) \
		exec = resolve_execute_segment<W>(cpu, pc);

#define UNCHECKED_JUMP()                                       \
	QUICK_EXEC_CHECK()                                         \
	d = &exec->decoder_cache()[pc / DecoderCache<W>::DIVISOR]; \
	BEGIN_BLOCK()                                              \
	EXECUTE_CURRENT()
#define OVERFLOW_CHECK()                            \
	if (UNLIKELY(COUNTER_OVERFLOWED(counter)))      \
		return RETURN_VALUES();

#define PERFORM_BRANCH()                \
	if constexpr (VERBOSE_JUMPS) {      \
		printf("Branch from 0x%lX to 0x%lX\n", \
			pc, pc + fi.signed_imm());  \
	}                                   \
	pc += fi.signed_imm();              \
	d += fi.signed_imm() / DecoderCache<W>::DIVISOR; \
	OVERFLOW_CHECK()                    \
	BEGIN_BLOCK()                       \
	EXECUTE_CURRENT()

#define PERFORM_FORWARD_BRANCH()        \
	if constexpr (VERBOSE_JUMPS) {      \
		printf("Fwd. Branch from 0x%lX to 0x%lX\n", \
			pc, pc + fi.signed_imm());  \
	}                                   \
	NEXT_BLOCK(fi.signed_imm(), false)

#define OVERFLOW_CHECKED_JUMP() \
	OVERFLOW_CHECK(); \
	UNCHECKED_JUMP();


namespace riscv
{
	static constexpr bool VERBOSE_JUMPS = riscv::verbose_branches_enabled;
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
	static constexpr bool FUZZING = true;
#else
	static constexpr bool FUZZING = false;
#endif

	template <int W>
	DecodedExecuteSegment<W>* resolve_execute_segment(CPU<W>& cpu, address_type<W>& pc)
	{
		// We have to store and restore PC here as there are
		// custom callbacks when changing segments that can
		// jump around.
		cpu.registers().pc = pc;
		// Change execute segment
		auto* exec = cpu.next_execute_segment();
		// Restore PC
		pc = cpu.registers().pc;
		return exec;
	}

	template <int W>
	using TcoRet = std::tuple<address_type<W>, uint64_t>;

	template <int W>
	using DecoderFunc = TcoRet<W>(*)(DecoderData<W>*, DecodedExecuteSegment<W>*, CPU<W> &, address_type<W> pc, uint64_t counter);
	namespace {
		template <int W>
		extern const DecoderFunc<W> computed_opcode[BYTECODES_MAX];
	}

#define DECODER()   (*d)
#define CPU()       cpu
#define REG(x)      cpu.reg(x)
#define REGISTERS() cpu.registers()
#define VECTORS()   cpu.registers().rvv()
#define MACHINE()   cpu.machine()

#define BYTECODES_OP_IMM
#  include "bytecode_impl.cpp"
#undef BYTECODES_OP_IMM

#define BYTECODES_LOAD_STORE
#  include "bytecode_impl.cpp"
#undef BYTECODES_LOAD_STORE

#define BYTECODES_BRANCH
#  include "bytecode_impl.cpp"
#undef BYTECODES_BRANCH

	INSTRUCTION(RV32I_BC_FUNCTION, execute_decoded_function)
	{
		auto handler = d->get_handler();
		handler(cpu, {d->instr});
		NEXT_INSTR();  
	}

	INSTRUCTION(RV32I_BC_SYSCALL, rv32i_syscall)
	{
		// Make the current PC visible
		cpu.registers().pc = pc;
		// Make the instruction counter(s) visible
		cpu.machine().set_instruction_counter(counter);
		// Invoke system call
		cpu.machine().system_call(cpu.reg(REG_ECALL));
		// Clone-like system calls can change PC
		if (UNLIKELY(pc != cpu.registers().pc))
		{
			pc = cpu.registers().pc;
			QUICK_EXEC_CHECK();
			d = &exec->decoder_cache()[pc / DecoderCache<W>::DIVISOR];
		}
		NEXT_BLOCK(4, true);
	}

	INSTRUCTION(RV32I_BC_FAST_JAL, rv32i_fast_jal)
	{
		if constexpr (VERBOSE_JUMPS) {
			printf("FAST_JAL PC 0x%lX => 0x%lX\n", (long)pc, (long)pc + d->instr);
		}
		NEXT_BLOCK((int32_t)d->instr, true);
	}
	INSTRUCTION(RV32I_BC_FAST_CALL, rv32i_fast_call)
	{
		if constexpr (VERBOSE_JUMPS)
		{
			printf("FAST_CALL PC 0x%lX => 0x%lX\n", pc, pc + d->instr);
		}
		cpu.reg(REG_RA) = pc + 4;
		NEXT_BLOCK((int32_t)d->instr, true);
	}
	INSTRUCTION(RV32I_BC_JAL, rv32i_jal)
	{
		VIEW_INSTR_AS(fi, FasterJtype);
		if constexpr (VERBOSE_JUMPS) {
			printf("JAL PC 0x%lX => 0x%lX\n", (long)pc, (long)pc + fi.offset);
		}
		if (fi.rd != 0)
			cpu.reg(fi.rd) = pc + 4;
		pc += fi.offset;
		OVERFLOW_CHECK();
		UNCHECKED_JUMP();
	}

#define BYTECODES_OP
#  include "bytecode_impl.cpp"
#undef BYTECODES_OP

	INSTRUCTION(RV32I_BC_JALR, rv32i_jalr)
	{
		VIEW_INSTR_AS(fi, FasterItype);
		// NOTE: if rs1 == rd, avoid clobber by storing address first
		const auto address = cpu.reg(fi.rs2) + fi.signed_imm();
		// jump to register + immediate
		if constexpr (VERBOSE_JUMPS) {
			printf("JALR PC 0x%lX => 0x%lX\n", pc, address);
		}
		// Alignment checks
		static constexpr auto alignment = compressed_enabled ? 0x1 : 0x3;
		if (UNLIKELY(address & alignment)) {
			cpu.trigger_exception(MISALIGNED_INSTRUCTION, address);
		}
		// Link *next* instruction (rd = PC + 4)
		if (fi.rs1 != 0) {
			cpu.reg(fi.rs1) = pc + 4;
		}
		pc = address;
		OVERFLOW_CHECK();
		UNCHECKED_JUMP();
	}

	INSTRUCTION(RV32I_BC_STOP, rv32i_stop)
	{
		(void) d;
		pc += 4; // Complete STOP instruction
		cpu.machine().stop();
		return RETURN_VALUES();
	}

#define BYTECODES_FLP
#  include "bytecode_impl.cpp"
#undef BYTECODES_FLP

	INSTRUCTION(RV32I_BC_FUNCBLOCK, execute_function_block) {
		VIEW_INSTR();
		auto handler = d->get_handler();
		handler(CPU(), instr);
		NEXT_BLOCK(instr.length(), true);
	}

	INSTRUCTION(RV32I_BC_SYSTEM, rv32i_system) {
		VIEW_INSTR();
		// Make the current PC visible
		cpu.registers().pc = pc;
		// Make the instruction counter visible
		cpu.machine().set_instruction_counter(counter);
		// Invoke SYSTEM
		cpu.machine().system(instr);
		// Restore PC in case it changed (supervisor)
		pc = cpu.registers().pc + 4;
		UNCHECKED_JUMP();
	}

	INSTRUCTION(RV32I_BC_TRANSLATOR, translated_function) {
#ifdef RISCV_BINARY_TRANSLATION
		VIEW_INSTR();
		// Make the current PC visible
		cpu.registers().pc = pc;
		// Make the instruction counter visible
		cpu.machine().set_instruction_counter(counter-1);
		// Invoke translated code
		exec->mapping_at(instr.whole)(cpu, instr);
		// Restore counter
		counter = cpu.machine().instruction_counter();
		// Translations are always full-length instructions (?)
		pc = cpu.registers().pc + 4;
		OVERFLOW_CHECK();
		UNCHECKED_JUMP();
#else
		(void)d;
		cpu.trigger_exception(FEATURE_DISABLED, pc);
#endif
	}

	INSTRUCTION(RV32I_BC_INVALID, execute_invalid)
	{
		cpu.trigger_exception(ILLEGAL_OPCODE, d->instr);
	}

#define BYTECODES_RARELY_USED
#  include "bytecode_impl.cpp"
#undef BYTECODES_RARELY_USED

	namespace
	{
		template <int W>
		static const DecoderFunc<W> computed_opcode[BYTECODES_MAX] = {
		[RV32I_BC_INVALID] = execute_invalid,
		[RV32I_BC_ADDI]    = rv32i_addi,
		[RV32I_BC_LI]      = rv32i_li,
		[RV32I_BC_MV]      = rv32i_mv,
		[RV32I_BC_SLLI]    = rv32i_slli,
		[RV32I_BC_SLTI]    = rv32i_slti,
		[RV32I_BC_SLTIU]   = rv32i_sltiu,
		[RV32I_BC_XORI]    = rv32i_xori,
		[RV32I_BC_SRLI]    = rv32i_srli,
		[RV32I_BC_SRAI]    = rv32i_srai,
		[RV32I_BC_ORI]     = rv32i_ori,
		[RV32I_BC_ANDI]    = rv32i_andi,

		[RV32I_BC_LUI]     = rv32i_lui,
		[RV32I_BC_AUIPC]   = rv32i_auipc,

		[RV32I_BC_LDB]     = rv32i_ldb,
		[RV32I_BC_LDBU]    = rv32i_ldbu,
		[RV32I_BC_LDH]     = rv32i_ldh,
		[RV32I_BC_LDHU]    = rv32i_ldhu,
		[RV32I_BC_LDW]     = rv32i_ldw,
		[RV32I_BC_LDWU]    = rv32i_ldwu,
		[RV32I_BC_LDD]     = rv32i_ldd,

		[RV32I_BC_STB]     = rv32i_stb,
		[RV32I_BC_STH]     = rv32i_sth,
		[RV32I_BC_STW]     = rv32i_stw,
		[RV32I_BC_STD]     = rv32i_std,

		[RV32I_BC_BEQ]     = rv32i_beq,
		[RV32I_BC_BNE]     = rv32i_bne,
		[RV32I_BC_BLT]     = rv32i_blt,
		[RV32I_BC_BGE]     = rv32i_bge,
		[RV32I_BC_BLTU]    = rv32i_bltu,
		[RV32I_BC_BGEU]    = rv32i_bgeu,
		[RV32I_BC_BEQ_FW]  = rv32i_beq_fw,
		[RV32I_BC_BNE_FW]  = rv32i_bne_fw,

		[RV32I_BC_JAL]     = rv32i_jal,
		[RV32I_BC_JALR]    = rv32i_jalr,
		[RV32I_BC_FAST_JAL] = rv32i_fast_jal,
		[RV32I_BC_FAST_CALL] = rv32i_fast_call,

		[RV32I_BC_OP_ADD]  = rv32i_op_add,
		[RV32I_BC_OP_SUB]  = rv32i_op_sub,
		[RV32I_BC_OP_SLL]  = rv32i_op_sll,
		[RV32I_BC_OP_SLT]  = rv32i_op_slt,
		[RV32I_BC_OP_SLTU] = rv32i_op_sltu,
		[RV32I_BC_OP_XOR]  = rv32i_op_xor,
		[RV32I_BC_OP_SRL]  = rv32i_op_srl,
		[RV32I_BC_OP_OR]   = rv32i_op_or,
		[RV32I_BC_OP_AND]  = rv32i_op_and,
		[RV32I_BC_OP_MUL]  = rv32i_op_mul,
		[RV32I_BC_OP_DIV]  = rv32i_op_div,
		[RV32I_BC_OP_DIVU] = rv32i_op_divu,
		[RV32I_BC_OP_REM]  = rv32i_op_rem,
		[RV32I_BC_OP_REMU] = rv32i_op_remu,
		[RV32I_BC_OP_SRA]  = rv32i_op_sra,
		[RV32I_BC_OP_ADD_UW] = rv32i_op_add_uw,
		[RV32I_BC_OP_ZEXT_H] = rv32i_op_zext_h,
		[RV32I_BC_OP_SH1ADD] = rv32i_op_sh1add,
		[RV32I_BC_OP_SH2ADD] = rv32i_op_sh2add,
		[RV32I_BC_OP_SH3ADD] = rv32i_op_sh3add,

		[RV32I_BC_SEXT_B] = rv32i_sext_b,
		[RV32I_BC_SEXT_H] = rv32i_sext_h,
		[RV32I_BC_BSETI]  = rv32i_bseti,
		[RV32I_BC_BEXTI]  = rv32i_bexti,

		[RV64I_BC_ADDIW]  = rv64i_addiw,
		[RV64I_BC_SRLIW]  = rv64i_srliw,
		[RV64I_BC_SRAIW]  = rv64i_sraiw,
		[RV64I_BC_OP_ADDW] = rv64i_op_addw,
		[RV64I_BC_OP_SH1ADD_UW] = rv64i_op_sh1add_uw,
		[RV64I_BC_OP_SH2ADD_UW] = rv64i_op_sh2add_uw,

#ifdef RISCV_EXT_COMPRESSED
		[RV32C_BC_ADDI]     = rv32c_addi,
		[RV32C_BC_LI]       = rv32c_addi,
		[RV32C_BC_MV]       = rv32c_mv,
		[RV32C_BC_SLLI]     = rv32c_slli,
		[RV32C_BC_BEQZ]     = rv32c_beqz,
		[RV32C_BC_BNEZ]     = rv32c_bnez,
		[RV32C_BC_JMP]      = rv32c_jmp,
		[RV32C_BC_JR]       = rv32c_jr,
		[RV32C_BC_JAL_ADDIW]= rv32c_jal_addiw,
		[RV32C_BC_JALR]     = rv32c_jalr,
		[RV32C_BC_LDD]      = rv32c_ldd,
		[RV32C_BC_STD]      = rv32c_std,
		[RV32C_BC_FUNCTION] = rv32c_func,
		[RV32C_BC_JUMPFUNC] = rv32c_jumpfunc,
#endif

		[RV32I_BC_SYSCALL] = rv32i_syscall,
		[RV32I_BC_STOP]    = rv32i_stop,
		[RV32I_BC_NOP]     = rv32i_nop,

		[RV32F_BC_FLW]     = rv32i_flw,
		[RV32F_BC_FLD]     = rv32i_fld,
		[RV32F_BC_FSW]     = rv32i_fsw,
		[RV32F_BC_FSD]     = rv32i_fsd,
		[RV32F_BC_FADD]    = rv32f_fadd,
		[RV32F_BC_FSUB]    = rv32f_fsub,
		[RV32F_BC_FMUL]    = rv32f_fmul,
		[RV32F_BC_FDIV]    = rv32f_fdiv,
		[RV32F_BC_FMADD]   = rv32f_fmadd,
#ifdef RISCV_EXT_VECTOR
		[RV32V_BC_VLE32]   = rv32v_vle32,
		[RV32V_BC_VSE32]   = rv32v_vse32,
		[RV32V_BC_VFADD_VV] = rv32v_vfadd_vv,
		[RV32V_BC_VFMUL_VF] = rv32v_vfmul_vf,
#endif
		[RV32I_BC_FUNCTION] = execute_decoded_function,
		[RV32I_BC_FUNCBLOCK] = execute_function_block,
		[RV32I_BC_TRANSLATOR] = translated_function,
		[RV32I_BC_SYSTEM]  = rv32i_system,
		};
	}

	template <int W> inline RISCV_HOT_PATH()
	void CPU<W>::simulate_tco(uint64_t imax)
	{
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

		uint64_t pc = this->pc();
		uint64_t counter = machine().instruction_counter();

		DecodedExecuteSegment<W>* exec = this->m_exec;
		DecoderData<W>* exec_decoder = exec->decoder_cache();
		auto* d = &exec_decoder[pc / DecoderCache<W>::DIVISOR];
		auto& cpu = *this;

		BEGIN_BLOCK();

		auto [new_pc, new_counter] = EXECUTE_INSTR();

		cpu.registers().pc = new_pc;
		machine().set_instruction_counter(new_counter);
	} // CPU::simulate_tco()

	template <int W>
	void CPU<W>::simulate(uint64_t imax)
	{
		simulate_tco(imax);
	}

	template struct CPU<4>;
	template struct CPU<8>;
	INSTANTIATE_128_IF_ENABLED(CPU);
} // riscv
