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
extern "C" {
	bool riscv64gb_accurate_dispatch(void* cpu, uint64_t pc, uint64_t icounter, uint64_t maxcounter);
	bool riscv64gb_inaccurate_dispatch(
		void* cpu, void* exec, void* decoder, uint64_t pc, uint64_t current_begin, uint64_t current_end);
	uint32_t riscv64gb_cpu_relative_icounter;
	uint32_t riscv64gb_cpu_relative_imaxcounter;
	void*    riscv64gb_cpu_syscall_array;

	// RV32: dispatch_rv32.nasm
	uint32_t rv32asm_dispatch(void* ctx);
#define RV32ASM_HANDLERS(X) \
	X(fallback) X(stop) \
	X(addi) X(li) X(mv) X(slli) X(srli) X(srai) X(slti) X(sltiu) \
	X(xori) X(ori) X(andi) X(sext_b) X(sext_h) X(bseti) X(bexti) \
	X(lui) X(auipc) \
	X(op_add) X(op_sub) X(op_sll) X(op_srl) X(op_sra) X(op_slt) X(op_sltu) \
	X(op_xor) X(op_or) X(op_and) X(op_mul) X(op_zext_h) \
	X(op_div) X(op_divu) X(op_rem) X(op_remu) X(function) \
	X(op_sh1add) X(op_sh2add) X(op_sh3add) \
	X(beq) X(bne) X(beq_fw) X(bne_fw) X(blt) X(bge) X(bltu) X(bgeu) \
	X(jal) X(fast_jal) X(fast_call) X(jalr) \
	X(ldw) X(ldh) X(ldhu) X(ldb) X(ldbu) X(stw) X(sth) X(stb)
#define RV32ASM_C_HANDLERS(X) \
	X(c_addi) X(c_mv) X(c_slli) X(c_srli) X(c_andi) X(c_add) X(c_xor) X(c_or) \
	X(c_bnez) X(c_beqz) X(c_jmp) X(c_jal) X(c_jr) X(c_jalr) X(c_ldw) X(c_stw) \
	X(c_function)
	// Each handler symbol is an array with the address of every copy
	extern const uint32_t rv32asm_ncopies;
#define RV32ASM_DECLARE(name) extern void* const rv32asm_##name[];
	RV32ASM_HANDLERS(RV32ASM_DECLARE)
#ifdef RISCV_EXT_COMPRESSED
	RV32ASM_C_HANDLERS(RV32ASM_DECLARE)
#endif
}

namespace riscv {

// Must match the CTX_* offsets in dispatch_rv32.nasm
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
static_assert(offsetof(AsmDispatchContext, pc) == 72);
static_assert(offsetof(AsmDispatchContext, cpu) == 80);
enum : uint32_t { ASM_EXIT_STOP = 0, ASM_EXIT_FALLBACK = 1, ASM_EXIT_JUMP = 2, ASM_EXIT_EXCEPTION = 3 };

// An exception thrown by a handler that the assembly called. No exception
// may unwind through the assembly frames, so it is kept here and rethrown
// once the dispatch has returned.
static thread_local std::exception_ptr rv32asm_exception;

#ifdef RISCV_32I
extern "C" uint32_t rv32asm_execute(CPU<4>* cpu, uint8_t* handler, uint32_t instr) noexcept
{
	try {
		cpu->execute(*handler, instr);
	} catch (...) {
		rv32asm_exception = std::current_exception();
		return 2;
	}
	return cpu->current_execute_segment().is_stale() ? 1 : 0;
}
#endif

static void** rv32asm_table()
{
	// table[copy * 256 + bytecode] = copy `copy` of the handler
	static std::vector<void*> table = [] {
		const unsigned copies = rv32asm_ncopies;
		std::vector<void*> t(copies * 256);
		for (unsigned c = 0; c < copies; c++)
			for (unsigned bc = 0; bc < 256; bc++)
				t[c * 256 + bc] = rv32asm_fallback[c];
		#define H(bc, name) for (unsigned c = 0; c < copies; c++) t[c * 256 + (bc)] = rv32asm_##name[c];
		H(RV32I_BC_STOP, stop)
		H(RV32I_BC_ADDI, addi)   H(RV32I_BC_LI, li)       H(RV32I_BC_MV, mv)
		H(RV32I_BC_SLLI, slli)   H(RV32I_BC_SRLI, srli)   H(RV32I_BC_SRAI, srai)
		H(RV32I_BC_SLTI, slti)   H(RV32I_BC_SLTIU, sltiu) H(RV32I_BC_XORI, xori)
		H(RV32I_BC_ORI, ori)     H(RV32I_BC_ANDI, andi)
		H(RV32I_BC_SEXT_B, sext_b) H(RV32I_BC_SEXT_H, sext_h)
		H(RV32I_BC_BSETI, bseti) H(RV32I_BC_BEXTI, bexti)
		H(RV32I_BC_LUI, lui)     H(RV32I_BC_AUIPC, auipc)
		H(RV32I_BC_OP_ADD, op_add) H(RV32I_BC_OP_SUB, op_sub)
		H(RV32I_BC_OP_SLL, op_sll) H(RV32I_BC_OP_SRL, op_srl) H(RV32I_BC_OP_SRA, op_sra)
		H(RV32I_BC_OP_SLT, op_slt) H(RV32I_BC_OP_SLTU, op_sltu)
		H(RV32I_BC_OP_XOR, op_xor) H(RV32I_BC_OP_OR, op_or) H(RV32I_BC_OP_AND, op_and)
		H(RV32I_BC_OP_MUL, op_mul) H(RV32I_BC_OP_ZEXT_H, op_zext_h)
		H(RV32I_BC_OP_DIV, op_div) H(RV32I_BC_OP_DIVU, op_divu)
		H(RV32I_BC_OP_REM, op_rem) H(RV32I_BC_OP_REMU, op_remu)
		H(RV32I_BC_FUNCTION, function)
		H(RV32I_BC_OP_SH1ADD, op_sh1add) H(RV32I_BC_OP_SH2ADD, op_sh2add)
		H(RV32I_BC_OP_SH3ADD, op_sh3add)
		H(RV32I_BC_BEQ, beq)     H(RV32I_BC_BNE, bne)
		H(RV32I_BC_BEQ_FW, beq_fw) H(RV32I_BC_BNE_FW, bne_fw)
		H(RV32I_BC_BLT, blt)     H(RV32I_BC_BGE, bge)
		H(RV32I_BC_BLTU, bltu)   H(RV32I_BC_BGEU, bgeu)
		H(RV32I_BC_JAL, jal)     H(RV32I_BC_FAST_JAL, fast_jal)
		H(RV32I_BC_FAST_CALL, fast_call) H(RV32I_BC_JALR, jalr)
		H(RV32I_BC_LDW, ldw)     H(RV32I_BC_LDH, ldh)     H(RV32I_BC_LDHU, ldhu)
		H(RV32I_BC_LDB, ldb)     H(RV32I_BC_LDBU, ldbu)
		H(RV32I_BC_STW, stw)     H(RV32I_BC_STH, sth)     H(RV32I_BC_STB, stb)
#ifdef RISCV_EXT_COMPRESSED
		H(RV32C_BC_ADDI, c_addi) H(RV32C_BC_MV, c_mv)
		H(RV32C_BC_SLLI, c_slli) H(RV32C_BC_SRLI, c_srli) H(RV32C_BC_ANDI, c_andi)
		H(RV32C_BC_ADD, c_add)   H(RV32C_BC_XOR, c_xor)   H(RV32C_BC_OR, c_or)
		H(RV32C_BC_BNEZ, c_bnez) H(RV32C_BC_BEQZ, c_beqz) H(RV32C_BC_JMP, c_jmp)
		H(RV32C_BC_JAL_ADDIW, c_jal) H(RV32C_BC_JR, c_jr) H(RV32C_BC_JALR, c_jalr)
		H(RV32C_BC_LDW, c_ldw)   H(RV32C_BC_STW, c_stw)
		H(RV32C_BC_FUNCTION, c_function)
#endif
		#undef H
		return t;
	}();
	return table.data();
}

template <int W>
void CPU<W>::simulate_inaccurate(address_t pc) {
	if constexpr (W == 8) {
		static_assert(DecoderData<W>::SHIFT == 2,
			"DecoderCache SHIFT must be 1 for assembly-based dispatch");
		machine().set_instruction_counter(0);
		machine().set_max_instructions(UINT64_MAX);
		// Set the relative instruction counter for the inaccurate dispatch
		riscv64gb_cpu_relative_icounter = (uintptr_t)&machine().get_counters().first - (uintptr_t)this;
		riscv64gb_cpu_relative_imaxcounter = (uintptr_t)&machine().get_counters().second - (uintptr_t)this;
		riscv64gb_cpu_syscall_array = (void*)&machine().syscall_handlers[0];

		DecodedExecuteSegment<W> *exec = this->m_exec;
		address_t current_begin = exec->exec_begin();
		address_t current_end = exec->exec_end();
		DecoderData<W> *exec_decoder = exec->decoder_cache();

		// We need an execute segment matching current PC
		if (UNLIKELY(!(pc >= current_begin && pc < current_end)))
		{
			auto new_values = this->next_execute_segment(pc);
			exec = new_values.exec;
			pc = new_values.pc;
			current_begin = exec->exec_begin();
			current_end = exec->exec_end();
			exec_decoder = exec->decoder_cache();
		}

		DecoderData<W> *decoder = &exec_decoder[pc >> DecoderData<W>::SHIFT];
		pc += decoder->block_bytes();

		riscv64gb_inaccurate_dispatch(this, exec, decoder, pc, current_begin, current_end);

	} else if constexpr (W == 4) {
		machine().set_instruction_counter(0);
		machine().set_max_instructions(UINT64_MAX);

		AsmDispatchContext ctx;
		ctx.regs  = &registers().get(0);
		ctx.cpu   = this;
		ctx.table = rv32asm_table();

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
			// The arena boundaries can change in any system call
			auto& mem = machine().memory;
			if (mem.uses_flat_memory_arena()) {
				ctx.arena = mem.memory_arena_ptr();
				ctx.read_boundary  = mem.memory_arena_read_boundary();
				ctx.rodata_end     = mem.initial_rodata_end();
				ctx.write_boundary = mem.memory_arena_write_boundary();
			} else {
				ctx.arena = nullptr;
				ctx.read_boundary = ctx.rodata_end = ctx.write_boundary = 0;
			}
			ctx.pc = pc;

			switch (rv32asm_dispatch(&ctx)) {
			case ASM_EXIT_STOP:
				registers().pc = ctx.pc;
				return;
			case ASM_EXIT_JUMP:
				pc = ctx.pc;
				continue;
			case ASM_EXIT_EXCEPTION: {
				registers().pc = ctx.pc;
				auto except = std::move(rv32asm_exception);
				rv32asm_exception = nullptr;
				std::rethrow_exception(except);
			}
			default: { // ASM_EXIT_FALLBACK: execute one instruction in C++
				pc = ctx.pc;
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
