#include "../machine.hpp"
#include "../decoder_cache.hpp"
#include "../internal_common.hpp"
#include "../riscvbase.hpp"
#include "../rv32i_instr.hpp"
#include "../threaded_bytecodes.hpp"
#include <array>
#include <vector>
#include <exception>
#include <cstddef>

// The inaccurate dispatch (no instruction counting) for RV32 and RV64 is
// written in assembly: amd64/dispatch_rv.nasm, assembled once per width. The
// accurate dispatch is still the C++ threaded dispatch.

// Handlers in dispatch_rv.nasm. Each handler symbol is an array with the
// address of every copy (see handler replication in the assembly).
#define ASMDISPATCH_HANDLERS(X) \
	X(fallback) X(stop) \
	X(addi) X(li) X(mv) X(slli) X(srli) X(srai) X(slti) X(sltiu) \
	X(xori) X(ori) X(andi) X(sext_b) X(sext_h) X(bseti) X(bexti) \
	X(lui) X(auipc) \
	X(op_add) X(op_sub) X(op_sll) X(op_srl) X(op_sra) X(op_slt) X(op_sltu) \
	X(op_xor) X(op_or) X(op_and) X(op_mul) X(op_zext_h) \
	X(op_div) X(op_divu) X(op_rem) X(op_remu) \
	X(op_sh1add) X(op_sh2add) X(op_sh3add) \
	X(function) X(funcblock) X(syscall) X(dyncall) X(system) X(step) \
	X(beq) X(bne) X(beq_fw) X(bne_fw) X(blt) X(bge) X(bltu) X(bgeu) \
	X(jal) X(fast_jal) X(fast_call) X(jalr) \
	X(ldw) X(ldh) X(ldhu) X(ldb) X(ldbu) X(stw) X(sth) X(stb) \
	X(flw) X(fld) X(fsw) X(fsd) X(fadd) X(fsub) X(fmul) X(fdiv) X(fmadd)
#define ASMDISPATCH_RV64_HANDLERS(X) \
	X(addiw) X(slliw) X(srliw) X(sraiw) \
	X(op_addw) X(op_subw) X(op_mulw) X(op_add_uw) X(op_sh1add_uw) X(op_sh2add_uw) \
	X(ldwu) X(ldd) X(std)
#define ASMDISPATCH_C_HANDLERS(X) \
	X(c_addi) X(c_mv) X(c_slli) X(c_srli) X(c_andi) X(c_add) X(c_xor) X(c_or) \
	X(c_bnez) X(c_beqz) X(c_jmp) X(c_jal_addiw) X(c_jr) X(c_jalr) X(c_ldw) X(c_stw) \
	X(c_function)
#define ASMDISPATCH_RV64_C_HANDLERS(X) \
	X(c_ldd) X(c_std)

extern "C" {
#ifdef RISCV_32I
	uint32_t rv32asm_dispatch(void* ctx);
	extern const uint32_t rv32asm_ncopies;
#define RV32ASM_DECLARE(name) extern void* const rv32asm_##name[];
	ASMDISPATCH_HANDLERS(RV32ASM_DECLARE)
#ifdef RISCV_EXT_COMPRESSED
	ASMDISPATCH_C_HANDLERS(RV32ASM_DECLARE)
#endif
#endif
#ifdef RISCV_64I
	uint32_t rv64asm_dispatch(void* ctx);
	extern const uint32_t rv64asm_ncopies;
#define RV64ASM_DECLARE(name) extern void* const rv64asm_##name[];
	ASMDISPATCH_HANDLERS(RV64ASM_DECLARE)
	ASMDISPATCH_RV64_HANDLERS(RV64ASM_DECLARE)
#ifdef RISCV_EXT_COMPRESSED
	ASMDISPATCH_C_HANDLERS(RV64ASM_DECLARE)
	ASMDISPATCH_RV64_C_HANDLERS(RV64ASM_DECLARE)
#endif
#endif
}

namespace riscv {

// Must match the CTX_* offsets in dispatch_rv.nasm
struct AsmDispatchContext {
	void*    regs;
	void*    decoder;
	uint64_t begin;
	uint64_t length;
	void*    arena;
	uint64_t read_boundary;
	uint64_t rodata_end;
	uint64_t write_boundary;
	void**   table;
	uint64_t pc;
	void*    cpu;
};
static_assert(offsetof(AsmDispatchContext, read_boundary) == 40);
static_assert(offsetof(AsmDispatchContext, table) == 64);
static_assert(offsetof(AsmDispatchContext, pc) == 72);
static_assert(offsetof(AsmDispatchContext, cpu) == 80);
static_assert(sizeof(DecoderData<4>) == 8 && sizeof(DecoderData<8>) == 8);

// Exit codes of the dispatch, also returned by asm_call
enum : uint32_t {
	ASM_EXIT_STOP = 0, ASM_EXIT_FALLBACK = 1, ASM_EXIT_JUMP = 2, ASM_EXIT_EXCEPTION = 3,
	ASM_CALL_NEXT = 4, // Only asm_call: continue with the next instruction
	ASM_CALL_GOTO = 5, // Only asm_call: continue at ctx->pc
};
// KIND_* in dispatch_rv.nasm
enum : uint32_t {
	ASM_KIND_FUNCTION = 0, ASM_KIND_C_FUNCTION, ASM_KIND_SYSCALL,
	ASM_KIND_DYNCALL, ASM_KIND_SYSTEM, ASM_KIND_FUNCBLOCK, ASM_KIND_STEP,
};
// offsetof(Registers<W>, m_regfl), FPOFF in dispatch_rv.nasm
template <int W> static constexpr ptrdiff_t ASM_FPOFF = (W == 4) ? 136 : 272;

// An exception thrown by C++ code that the assembly called. No exception
// may unwind through the assembly frames, so it is kept here and rethrown
// once the dispatch has returned.
static thread_local std::exception_ptr asm_exception;

template <int W>
static void asm_refresh_arena(Memory<W>& mem, AsmDispatchContext* ctx)
{
	if (mem.uses_flat_memory_arena()) {
		ctx->arena = mem.memory_arena_ptr();
		ctx->read_boundary  = mem.memory_arena_read_boundary();
		ctx->rodata_end     = mem.initial_rodata_end();
		ctx->write_boundary = mem.memory_arena_write_boundary();
	} else {
		// Every load and store fails the bounds check and falls back
		ctx->arena = nullptr;
		ctx->read_boundary = ctx->rodata_end = ctx->write_boundary = 0;
	}
}

// Executes the current decoder entry in C++ on behalf of the assembly.
// The semantics follow the tail-call dispatch (tailcall_dispatch.cpp).
template <int W>
static uint32_t asm_call(AsmDispatchContext* ctx, DecoderData<W>* entry, uint32_t kind) noexcept
{
	using address_t = address_type<W>;
	auto& cpu = *static_cast<CPU<W>*>(ctx->cpu);
	auto& machine = cpu.machine();
	auto& regs = cpu.registers();
	const address_t pc = address_t(entry - static_cast<DecoderData<W>*>(ctx->decoder)) << DecoderData<W>::SHIFT;
	// Make the current PC visible
	regs.pc = pc;
	address_t next_pc = pc + 4;
	bool jumped = false;
	try {
		switch (kind) {
		case ASM_KIND_C_FUNCTION:
			next_pc = pc + 2;
			[[fallthrough]];
		case ASM_KIND_FUNCTION:
		case ASM_KIND_DYNCALL:
			cpu.execute(entry->m_handler, entry->instr);
			break;
		case ASM_KIND_FUNCBLOCK: // See pcrel.cpp
			cpu.execute(entry->m_handler, entry->instr);
			next_pc = pc + rv32i_instruction{entry->instr}.length();
			jumped = true;
			break;
		case ASM_KIND_SYSCALL:
			machine.system_call(regs.get(REG_ECALL));
			// System calls are always full-length instructions
			if (regs.pc != pc || machine.stopped()) {
				next_pc = regs.pc + 4;
				jumped = true;
			}
			break;
		case ASM_KIND_STEP: // Always a 4-byte instruction, see the handler
			cpu.step_one(false);
			jumped = (regs.pc != next_pc);
			next_pc = regs.pc;
			break;
		case ASM_KIND_SYSTEM:
			machine.system(rv32i_instruction{entry->instr});
			// A changed PC is where to continue (not after it)
			if (regs.pc != pc || machine.stopped()) {
				next_pc = regs.pc;
				jumped = true;
			}
			break;
		}
	} catch (...) {
		asm_exception = std::current_exception();
		ctx->pc = regs.pc;
		return ASM_EXIT_EXCEPTION;
	}
	asm_refresh_arena(machine.memory, ctx);
	ctx->pc = next_pc;
	if (UNLIKELY(machine.stopped()))
		return ASM_EXIT_STOP;
	// A (nested) call may have left another execute segment, or the code
	// may have been changed: let C++ find the right segment.
	auto& exec = cpu.current_execute_segment();
	if (UNLIKELY(exec.decoder_cache() != ctx->decoder || exec.is_stale()))
		return ASM_EXIT_JUMP;
	return jumped ? ASM_CALL_GOTO : ASM_CALL_NEXT;
}

template <int W>
struct AsmDispatch;

#define ASMDISPATCH_FILL(T, P) \
	H(RV32I_BC_STOP, P##stop) \
	H(RV32I_BC_ADDI, P##addi)   H(RV32I_BC_LI, P##li)       H(RV32I_BC_MV, P##mv) \
	H(RV32I_BC_SLLI, P##slli)   H(RV32I_BC_SRLI, P##srli)   H(RV32I_BC_SRAI, P##srai) \
	H(RV32I_BC_SLTI, P##slti)   H(RV32I_BC_SLTIU, P##sltiu) H(RV32I_BC_XORI, P##xori) \
	H(RV32I_BC_ORI, P##ori)     H(RV32I_BC_ANDI, P##andi) \
	H(RV32I_BC_SEXT_B, P##sext_b) H(RV32I_BC_SEXT_H, P##sext_h) \
	H(RV32I_BC_BSETI, P##bseti) H(RV32I_BC_BEXTI, P##bexti) \
	H(RV32I_BC_LUI, P##lui)     H(RV32I_BC_AUIPC, P##auipc) \
	H(RV32I_BC_OP_ADD, P##op_add) H(RV32I_BC_OP_SUB, P##op_sub) \
	H(RV32I_BC_OP_SLL, P##op_sll) H(RV32I_BC_OP_SRL, P##op_srl) H(RV32I_BC_OP_SRA, P##op_sra) \
	H(RV32I_BC_OP_SLT, P##op_slt) H(RV32I_BC_OP_SLTU, P##op_sltu) \
	H(RV32I_BC_OP_XOR, P##op_xor) H(RV32I_BC_OP_OR, P##op_or) H(RV32I_BC_OP_AND, P##op_and) \
	H(RV32I_BC_OP_MUL, P##op_mul) H(RV32I_BC_OP_ZEXT_H, P##op_zext_h) \
	H(RV32I_BC_OP_DIV, P##op_div) H(RV32I_BC_OP_DIVU, P##op_divu) \
	H(RV32I_BC_OP_REM, P##op_rem) H(RV32I_BC_OP_REMU, P##op_remu) \
	H(RV32I_BC_OP_SH1ADD, P##op_sh1add) H(RV32I_BC_OP_SH2ADD, P##op_sh2add) \
	H(RV32I_BC_OP_SH3ADD, P##op_sh3add) \
	H(RV32I_BC_FUNCTION, P##function) H(RV32I_BC_FUNCBLOCK, P##funcblock) \
	H(RV32I_BC_SYSCALL, P##syscall) H(RV32I_BC_DYNCALL, P##dyncall) \
	H(RV32I_BC_SYSTEM, P##system) \
	H(RV32I_BC_BEQ, P##beq)     H(RV32I_BC_BNE, P##bne) \
	H(RV32I_BC_BEQ_FW, P##beq_fw) H(RV32I_BC_BNE_FW, P##bne_fw) \
	H(RV32I_BC_BLT, P##blt)     H(RV32I_BC_BGE, P##bge) \
	H(RV32I_BC_BLTU, P##bltu)   H(RV32I_BC_BGEU, P##bgeu) \
	H(RV32I_BC_JAL, P##jal)     H(RV32I_BC_FAST_JAL, P##fast_jal) \
	H(RV32I_BC_FAST_CALL, P##fast_call) H(RV32I_BC_JALR, P##jalr) \
	H(RV32I_BC_LDW, P##ldw)     H(RV32I_BC_LDH, P##ldh)     H(RV32I_BC_LDHU, P##ldhu) \
	H(RV32I_BC_LDB, P##ldb)     H(RV32I_BC_LDBU, P##ldbu) \
	H(RV32I_BC_STW, P##stw)     H(RV32I_BC_STH, P##sth)     H(RV32I_BC_STB, P##stb) \
	H(RV32F_BC_FLD, P##fld)     H(RV32F_BC_FSW, P##fsw)     H(RV32F_BC_FSD, P##fsd) \
	/* NaN-boxing and FCSR emulation are left to C++ (CPU::step_one) */ \
	if constexpr (!nanboxing) { H(RV32F_BC_FLW, P##flw) } \
	else { H(RV32F_BC_FLW, P##step) } \
	if constexpr (!nanboxing && !fcsr_emulation) { \
		H(RV32F_BC_FADD, P##fadd) H(RV32F_BC_FSUB, P##fsub) \
		H(RV32F_BC_FMUL, P##fmul) H(RV32F_BC_FDIV, P##fdiv) \
		H(RV32F_BC_FMADD, P##fmadd) \
	} else { \
		H(RV32F_BC_FADD, P##step) H(RV32F_BC_FSUB, P##step) \
		H(RV32F_BC_FMUL, P##step) H(RV32F_BC_FDIV, P##step) \
		H(RV32F_BC_FMADD, P##step) \
	}
#define ASMDISPATCH_FILL_C(P) \
	H(RV32C_BC_ADDI, P##c_addi) H(RV32C_BC_MV, P##c_mv) \
	H(RV32C_BC_SLLI, P##c_slli) H(RV32C_BC_SRLI, P##c_srli) H(RV32C_BC_ANDI, P##c_andi) \
	H(RV32C_BC_ADD, P##c_add)   H(RV32C_BC_XOR, P##c_xor)   H(RV32C_BC_OR, P##c_or) \
	H(RV32C_BC_BNEZ, P##c_bnez) H(RV32C_BC_BEQZ, P##c_beqz) H(RV32C_BC_JMP, P##c_jmp) \
	H(RV32C_BC_JAL_ADDIW, P##c_jal_addiw) H(RV32C_BC_JR, P##c_jr) H(RV32C_BC_JALR, P##c_jalr) \
	H(RV32C_BC_LDW, P##c_ldw)   H(RV32C_BC_STW, P##c_stw) \
	H(RV32C_BC_FUNCTION, P##c_function)

// table[copy * 256 + bytecode] = copy `copy` of the handler
#define H(bc, sym) for (unsigned c = 0; c < copies; c++) t[c * 256 + (bc)] = sym[c];

#ifdef RISCV_32I
template <>
struct AsmDispatch<4> {
	static uint32_t dispatch(AsmDispatchContext* ctx) { return rv32asm_dispatch(ctx); }
	static std::vector<void*> make_table() {
		const unsigned copies = rv32asm_ncopies;
		std::vector<void*> t(copies * 256);
		for (unsigned c = 0; c < copies; c++)
			for (unsigned bc = 0; bc < 256; bc++)
				t[c * 256 + bc] = rv32asm_fallback[c];
		ASMDISPATCH_FILL(4, rv32asm_)
#ifdef RISCV_EXT_COMPRESSED
		ASMDISPATCH_FILL_C(rv32asm_)
#endif
		return t;
	}
};
extern "C" uint32_t rv32asm_call(AsmDispatchContext* ctx, DecoderData<4>* entry, uint32_t kind) noexcept
{
	return asm_call<4>(ctx, entry, kind);
}
#endif
#ifdef RISCV_64I
template <>
struct AsmDispatch<8> {
	static uint32_t dispatch(AsmDispatchContext* ctx) { return rv64asm_dispatch(ctx); }
	static std::vector<void*> make_table() {
		const unsigned copies = rv64asm_ncopies;
		std::vector<void*> t(copies * 256);
		for (unsigned c = 0; c < copies; c++)
			for (unsigned bc = 0; bc < 256; bc++)
				t[c * 256 + bc] = rv64asm_fallback[c];
		ASMDISPATCH_FILL(8, rv64asm_)
		H(RV64I_BC_ADDIW, rv64asm_addiw)
		H(RV64I_BC_SLLIW, rv64asm_slliw) H(RV64I_BC_SRLIW, rv64asm_srliw)
		H(RV64I_BC_SRAIW, rv64asm_sraiw)
		H(RV64I_BC_OP_ADDW, rv64asm_op_addw) H(RV64I_BC_OP_SUBW, rv64asm_op_subw)
		H(RV64I_BC_OP_MULW, rv64asm_op_mulw) H(RV64I_BC_OP_ADD_UW, rv64asm_op_add_uw)
		H(RV64I_BC_OP_SH1ADD_UW, rv64asm_op_sh1add_uw)
		H(RV64I_BC_OP_SH2ADD_UW, rv64asm_op_sh2add_uw)
		H(RV32I_BC_LDWU, rv64asm_ldwu) H(RV32I_BC_LDD, rv64asm_ldd)
		H(RV32I_BC_STD, rv64asm_std)
#ifdef RISCV_EXT_COMPRESSED
		ASMDISPATCH_FILL_C(rv64asm_)
		H(RV32C_BC_LDD, rv64asm_c_ldd) H(RV32C_BC_STD, rv64asm_c_std)
#endif
		return t;
	}
};
extern "C" uint32_t rv64asm_call(AsmDispatchContext* ctx, DecoderData<8>* entry, uint32_t kind) noexcept
{
	return asm_call<8>(ctx, entry, kind);
}
#endif
#undef H

// The assembly uses BMI2, AVX and FMA3
static bool asm_dispatch_supported()
{
	static const bool supported = __builtin_cpu_supports("bmi2")
		&& __builtin_cpu_supports("avx") && __builtin_cpu_supports("fma");
	return supported;
}

template <int W>
void CPU<W>::simulate_inaccurate(address_t pc)
{
	if constexpr (W == 4 || W == 8) {
		if (UNLIKELY(!asm_dispatch_supported())) {
			// The accurate dispatch, with a counter that never runs out
			machine().set_max_instructions(UINT64_MAX);
			this->simulate(pc, 0, UINT64_MAX);
			return;
		}
		static void** const table = [] {
			static std::vector<void*> t = AsmDispatch<W>::make_table();
			return t.data();
		}();

		machine().set_instruction_counter(0);
		machine().set_max_instructions(UINT64_MAX);

		AsmDispatchContext ctx;
		ctx.regs  = &registers().get(0);
		ctx.cpu   = this;
		ctx.table = table;
		if (UNLIKELY((char*)&registers().getfl(0) - (char*)ctx.regs != ASM_FPOFF<W>))
			throw MachineException(FEATURE_DISABLED, "Assembly dispatch: unexpected register file layout");

		while (true) {
			DecodedExecuteSegment<W>* exec = this->m_exec;
			if (UNLIKELY(!(pc >= exec->exec_begin() && pc < exec->exec_end()) || exec->is_stale())) {
				auto new_values = this->next_execute_segment(pc);
				exec = new_values.exec;
				pc   = new_values.pc;
			}
			ctx.decoder = exec->decoder_cache();
			ctx.begin   = exec->exec_begin();
			ctx.length  = exec->exec_end() - exec->exec_begin();
			asm_refresh_arena(machine().memory, &ctx);
			ctx.pc = pc;

			switch (AsmDispatch<W>::dispatch(&ctx)) {
			case ASM_EXIT_STOP:
				registers().pc = address_t(ctx.pc);
				return;
			case ASM_EXIT_JUMP:
				pc = address_t(ctx.pc);
				continue;
			case ASM_EXIT_EXCEPTION: {
				registers().pc = address_t(ctx.pc);
				auto except = std::move(asm_exception);
				asm_exception = nullptr;
				std::rethrow_exception(except);
			}
			default: { // ASM_EXIT_FALLBACK: execute one instruction in C++
				pc = address_t(ctx.pc);
				const auto& entry = exec->decoder_cache()[pc >> DecoderData<W>::SHIFT];
				if (entry.get_bytecode() == RV32I_BC_INVALID) {
					// Same as execute_invalid in cpu_inaccurate_dispatch.cpp: the
					// decoder also uses INVALID for instructions it refuses, such as
					// branches out of the execute segment, so only undecoded entries
					// may be executed from memory.
					if (entry.instr == 0 && exec->is_within(pc)) {
						pc = this->simulate_undecoded(pc);
						if (machine().stopped())
							return;
						continue;
					}
					registers().pc = pc;
					trigger_exception(ILLEGAL_OPCODE, entry.instr);
				}
				registers().pc = pc;
				this->step_one(false);
			}
				if (machine().stopped())
					return;
				pc = registers().pc;
				continue;
			}
		}
	} else {
		// For 128-bit, we don't have an inaccurate dispatch
		throw MachineException(FEATURE_DISABLED, "Inaccurate dispatch is not implemented for this CPU width");
	}
}

	INSTANTIATE_32_IF_ENABLED(CPU);
	INSTANTIATE_64_IF_ENABLED(CPU);
} // riscv
