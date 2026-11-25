#pragma once
// The C++ side of the hand-written dispatches: amd64/dispatch_rv.nasm
// (amd64/asm_dispatch.cpp) and wasm/dispatch_rv64.py (wasm/wasm_dispatch.cpp).
//
// Both keep the hot state in registers (or locals) and only call into C++
// through asm_call(), for system calls, FUNCTION entries, native code and
// the fallback for anything their fast paths do not handle. asm_simulate()
// is the outer loop around them: it finds the execute segment, enters the
// dispatch and handles whatever made it return. The semantics follow
// cpu_dispatch.cpp (Counting, CPU::simulate) and cpu_inaccurate_dispatch.cpp
// (!Counting, CPU::simulate_inaccurate).
#include "machine.hpp"
#include "decoder_cache.hpp"
#include "internal_common.hpp"
#include "riscvbase.hpp"
#include "rv32i_instr.hpp"
#include "threaded_bytecodes.hpp"
#include <cstddef>
#include <exception>

namespace riscv {

// Must match the CTX_* offsets in dispatch_rv.nasm and CTX in dispatch_rv64.py
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
	uint64_t counter;     // Counting dispatch only
	uint64_t max_counter; // Counting dispatch only
	uint8_t  enter_count; // Counting dispatch: count the block it enters
	uint32_t budget;      // WebAssembly: taken backward branches and jumps until ASM_EXIT_YIELD
	// WebAssembly: asm_call for this dispatch, called through this pointer
	uint32_t (*call)(AsmDispatchContext*, DecoderData<8>*, uint32_t);
	// WebAssembly: FUNCTION entries call their instruction handler directly
	const void* handlers;     // DecoderData<W>::get_handlers()
	const bool* stale;        // The current execute segment is stale
	uint64_t*   counter_ptr;  // Machine::instruction_counter(), kept current across such calls
	uint32_t    instr;        // The instruction_format argument (passed by reference in wasm32)
	// An exception thrown by C++ code that the dispatch called (asm_call).
	// No exception may unwind through the assembly frames, so it is kept
	// here and rethrown once the dispatch has returned.
	std::exception_ptr* exception;
};

// Exit codes of the dispatch, also returned by asm_call
enum : uint32_t {
	ASM_EXIT_STOP = 0,     // STOP, or (inaccurate) the machine was stopped
	ASM_EXIT_REENTER = 1,  // Continue at ctx->pc without counting a new block
	ASM_EXIT_JUMP = 2,     // Continue at ctx->pc, which begins a block
	ASM_EXIT_EXCEPTION = 3,
	ASM_CALL_NEXT = 4,     // Only asm_call: continue with the next instruction
	ASM_CALL_GOTO = 5,     // Only asm_call: continue at ctx->pc
	ASM_EXIT_OVERFLOW = 6, // Counting: the counter reached the max, at ctx->pc
	ASM_EXIT_YIELD = 7,    // Like ASM_EXIT_JUMP, when ctx->budget ran out (see Dispatch::on_yield)
};
// KIND_* in dispatch_rv.nasm and dispatch_rv64.py
enum : uint32_t {
	ASM_KIND_FUNCTION = 0, ASM_KIND_C_FUNCTION, ASM_KIND_SYSCALL,
	ASM_KIND_DYNCALL, ASM_KIND_SYSTEM, ASM_KIND_FUNCBLOCK, ASM_KIND_STEP,
	ASM_KIND_INVALID, ASM_KIND_TRANSLATOR, ASM_KIND_ASMJIT, ASM_KIND_LIVEPATCH,
	ASM_KIND_C_STEP, // ASM_KIND_STEP for a 2-byte instruction
};
// offsetof(Registers<W>, m_regfl), FPOFF in dispatch_rv.nasm
template <int W> static constexpr ptrdiff_t ASM_FPOFF = (W == 4) ? 136 : 272;

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

[[maybe_unused]] [[noreturn]] static void asm_rethrow_current(auto& cpu)
{
	const auto except = cpu.current_exception();
	cpu.clear_current_exception();
	std::rethrow_exception(except);
}

// Executes the current decoder entry in C++ on behalf of the dispatch.
template <int W, bool Counting>
static uint32_t asm_call(AsmDispatchContext* ctx, DecoderData<W>* entry, uint32_t kind) noexcept
{
	using address_t = address_type<W>;
	auto& cpu = *static_cast<CPU<W>*>(ctx->cpu);
	auto& machine = cpu.machine();
	auto& regs = cpu.registers();
	auto& exec = cpu.current_execute_segment();
	auto* decoder = static_cast<DecoderData<W>*>(ctx->decoder);
	const address_t pc = address_t(entry - decoder) << DecoderData<W>::SHIFT;
	// Make the current PC and the instruction counters visible
	regs.pc = pc;
	if constexpr (Counting) {
		machine.set_instruction_counter(ctx->counter);
		machine.set_max_instructions(ctx->max_counter);
	}
	address_t next_pc = pc + 4;
	bool jumped = false;     // Continue at next_pc, which begins a block
	bool next_block = false; // The next instruction begins a block
	try {
		switch (kind) {
		case ASM_KIND_FUNCTION:
		case ASM_KIND_DYNCALL:
			cpu.execute(entry->m_handler, entry->instr);
			break;
		case ASM_KIND_C_FUNCTION:
			cpu.execute(entry->m_handler, entry->instr);
			next_pc = pc + 2;
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
			next_block = true;
			break;
		case ASM_KIND_C_STEP: // The fallback of a 2-byte instruction
			next_pc = pc + 2;
			[[fallthrough]];
		case ASM_KIND_STEP: // A 4-byte instruction, see the handler
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
			next_block = true;
			break;
		case ASM_KIND_INVALID:
			// The decoder also uses INVALID for instructions it refuses, such as
			// branches out of the execute segment, so only undecoded entries may
			// be executed from memory. See execute_invalid in cpu_dispatch.cpp.
			if (entry->get_bytecode() == RV32I_BC_INVALID && entry->instr == 0 && exec.is_within(pc)) {
				next_pc = cpu.simulate_undecoded(pc);
				jumped = true;
				break;
			}
			cpu.trigger_exception(ILLEGAL_OPCODE, entry->instr);
			break;
#ifdef RISCV_BINARY_TRANSLATION
		case ASM_KIND_TRANSLATOR: {
			// Undo what the dispatch counted for this entry, as the translated code
			// counts itself. The counter can be 0 here (an entered vmcall, or any
			// return from code translated with ignore_instruction_limit) while the
			// entry counted 0 (icount with RVC) or nothing (a re-entry), so it must
			// not wrap: translated code can return the counter it was given.
			uint64_t counter = 0;
			if constexpr (Counting) {
				const uint64_t counted = entry->instruction_count();
				counter = (ctx->counter > counted) ? ctx->counter - counted : 0;
			}
			uint64_t max     = Counting ? ctx->max_counter : ~0ULL;
			address_t tpc = pc;
			const auto* e = entry;
			while (true) {
				const auto results = exec.unchecked_mapping_at(e->instr)(cpu, counter, max, tpc);
#  ifdef RISCV_LIBTCC
				if (UNLIKELY(cpu.has_current_exception()))
					asm_rethrow_current(cpu);
#  endif
				tpc = regs.pc;
				counter = results.counter;
				max     = results.max_counter;
				if (!(counter < max) || !(tpc >= exec.exec_begin() && tpc < exec.exec_end())
					|| exec.is_stale())
					break;
				e = &decoder[tpc >> DecoderData<W>::SHIFT];
				if (e->get_bytecode() != RV32I_BC_TRANSLATOR)
					break;
			}
			machine.set_instruction_counter(counter);
			machine.set_max_instructions(max);
			next_pc = tpc;
			jumped = true;
			break;
		}
#endif
#ifdef RISCV_ASMJIT
		case ASM_KIND_ASMJIT: {
			AjState<W> state {
				(Counting && ctx->counter > entry->instruction_count())
					? ctx->counter - entry->instruction_count() : 0,
				Counting ? ctx->max_counter : ~0ULL, pc };
			state.file_refault_pending = &machine.memory.file_refault_pending_ref();
			const auto* e = entry;
			do {
				exec.unchecked_asmjit_mapping_at(e->instr)(cpu, &state);
				if (UNLIKELY(state.max_counter == 0 && cpu.has_current_exception()))
					asm_rethrow_current(cpu);
				if (!(state.pc >= exec.exec_begin() && state.pc < exec.exec_end()))
					break;
				e = &decoder[state.pc >> DecoderData<W>::SHIFT];
			} while (state.counter < state.max_counter && e->get_bytecode() == RV32I_BC_ASMJIT);
			machine.set_instruction_counter(state.counter);
			machine.set_max_instructions(state.max_counter);
			next_pc = state.pc;
			jumped = true;
			break;
		}
#endif
		case ASM_KIND_LIVEPATCH:
#if defined(RISCV_BINARY_TRANSLATION) || defined(RISCV_ASMJIT)
			// Native code was activated in the background, and the segment
			// now uses the patched decoder cache. Re-enter at this instruction,
			// which belongs to a block that has already been counted.
#else
			// Without native code there is nothing to switch to
			entry->set_bytecode(RV32I_BC_INVALID);
			entry->set_invalid_handler();
#endif
			ctx->pc = pc;
			return ASM_EXIT_REENTER;
		default: // Bytecodes without a translator or asmjit build
			cpu.trigger_exception(ILLEGAL_OPCODE, entry->instr);
			break;
		}
	} catch (...) {
		*ctx->exception = std::current_exception();
		ctx->pc = regs.pc;
		if constexpr (Counting) {
			ctx->counter = machine.instruction_counter();
			ctx->max_counter = machine.max_instructions();
		}
		return ASM_EXIT_EXCEPTION;
	}
	asm_refresh_arena(machine.memory, ctx);
	ctx->pc = next_pc;
	if constexpr (Counting) {
		ctx->counter = machine.instruction_counter();
		ctx->max_counter = machine.max_instructions();
		// The dispatch checks the counter at ctx->pc and stops there, but only
		// where cpu_dispatch.cpp checks it. After a FUNCTION (or a STEP that did
		// not jump) it continues to the next check, even though the counter
		// may already have overflowed when the block was entered.
		if (UNLIKELY(ctx->counter >= ctx->max_counter)
			&& (jumped || next_block || kind == ASM_KIND_DYNCALL))
			return ASM_CALL_GOTO;
	} else {
		if (UNLIKELY(machine.stopped()))
			return ASM_EXIT_STOP;
	}
	// A (nested) call may have left another execute segment, or the code
	// may have been changed: let C++ find the right segment.
	auto& current = cpu.current_execute_segment();
	if (UNLIKELY(current.is_stale()))
		return ASM_EXIT_JUMP;
	if (UNLIKELY(current.decoder_cache() != ctx->decoder))
		return (jumped || next_block) ? ASM_EXIT_JUMP : ASM_EXIT_REENTER;
	return jumped ? ASM_CALL_GOTO : ASM_CALL_NEXT;
}

// The outer loop of both dispatches: find the execute segment, enter the
// dispatch, and handle whatever made it return. Counting follows
// cpu_dispatch.cpp: returns true when the machine stopped normally.
// Dispatch provides:
//   static void prepare(AsmDispatchContext&)  fill in what the backend needs
//   static uint32_t dispatch(AsmDispatchContext*)
//   static void on_yield(AsmDispatchContext&) after ASM_EXIT_YIELD
template <int W, bool Counting, typename Dispatch>
static bool asm_simulate(CPU<W>& cpu, address_type<W> pc, uint64_t counter, uint64_t max_counter)
{
	using address_t = address_type<W>;
	auto& machine = cpu.machine();
	auto& regs = cpu.registers();

	// Not a thread_local: with Emscripten's -fwasm-exceptions, assigning a
	// thread_local exception_ptr in a catch block traps (emsdk 6.0).
	std::exception_ptr exception;
	AsmDispatchContext ctx;
	ctx.exception = &exception;
	ctx.regs  = &regs.get(0);
	ctx.cpu   = &cpu;
	ctx.table = nullptr;
	ctx.budget = 0;
	ctx.call  = nullptr;
	ctx.handlers = DecoderData<W>::get_handlers();
	ctx.counter_ptr = &machine.get_counters().first;
	Dispatch::prepare(ctx);
	if (UNLIKELY((char*)&regs.getfl(0) - (char*)ctx.regs != ASM_FPOFF<W>))
		throw MachineException(FEATURE_DISABLED, "Assembly dispatch: unexpected register file layout");

	bool count_entry = true;
	while (true) {
		DecodedExecuteSegment<W>* exec = &cpu.current_execute_segment();
		if (UNLIKELY(!(pc >= exec->exec_begin() && pc < exec->exec_end()) || exec->is_stale())) {
			auto new_values = cpu.next_execute_segment(pc);
			exec = new_values.exec;
			pc   = new_values.pc;
			count_entry = true;
			if constexpr (Counting) {
				counter += new_values.penalty;
				if (UNLIKELY(counter >= max_counter)) {
					regs.pc = pc;
					machine.set_instruction_counter(counter);
					return max_counter == 0;
				}
			}
		}
		ctx.decoder = exec->decoder_cache();
		ctx.stale   = exec->stale_flag();
		ctx.begin   = exec->exec_begin();
		ctx.length  = exec->exec_end() - exec->exec_begin();
		asm_refresh_arena(machine.memory, &ctx);
		ctx.pc = pc;
		ctx.counter = counter;
		ctx.max_counter = max_counter;
		ctx.enter_count = count_entry;

		const uint32_t code = Dispatch::dispatch(&ctx);
		pc = address_t(ctx.pc);
		if constexpr (Counting) {
			counter = ctx.counter;
			max_counter = ctx.max_counter;
		}
		switch (code) {
		case ASM_EXIT_REENTER:
			count_entry = false;
			continue;
		case ASM_EXIT_YIELD:
			Dispatch::on_yield(ctx);
			[[fallthrough]];
		case ASM_EXIT_JUMP:
			// A jump checks the counter, like check_jump in cpu_dispatch.cpp
			if constexpr (Counting) {
				if (UNLIKELY(counter >= max_counter))
					break;
			}
			count_entry = true;
			continue;
		case ASM_EXIT_EXCEPTION: {
			regs.pc = pc;
			if constexpr (Counting)
				machine.set_instruction_counter(counter);
			std::rethrow_exception(std::move(exception));
		}
		case ASM_EXIT_STOP:
			regs.pc = pc;
			if constexpr (Counting)
				machine.set_instruction_counter(counter);
			return true;
		default: // ASM_EXIT_OVERFLOW
			break;
		}
		// The counter reached the max
		regs.pc = pc;
		machine.set_instruction_counter(counter);
		return max_counter == 0;
	}
}

} // riscv
