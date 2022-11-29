#include "machine.hpp"
#include "decoder_cache.hpp"
#include "instruction_counter.hpp"
#include "riscvbase.hpp"
#include "rv32i_instr.hpp"

namespace riscv
{
	template<int W> __attribute__((hot))
	void CPU<W>::simulate_fastsim(uint64_t imax)
	{
		// If we start outside of the fixed execute segment,
		// we can fall back to the precise simulator.
		if (UNLIKELY(!is_executable(this->pc()))) {
			this->next_execute_segment();
		}

		// In fastsim mode the instruction counter becomes a register
		// the function, and we only update m_counter in Machine on exit
		// When binary translation is enabled we cannot do this optimization.
		InstrCounter counter{machine()};

		if (imax != UINT64_MAX)
			machine().set_max_instructions(counter.value() + imax);
		else
			machine().set_max_instructions(UINT64_MAX);

restart_simulation:
		const auto* current_exec = this->m_exec;
		const auto current_begin = current_exec->exec_begin();
		const auto current_end = current_exec->exec_end();
		const auto* exec_decoder = current_exec->decoder_cache();

		auto pc = this->pc();
		do {
			// Retrieve handler directly from the instruction handler cache
			auto* decoder = &exec_decoder[pc / DecoderCache<W>::DIVISOR];
			// The number of instructions to run until we can check
			// if we ran out of instructions or PC changed.
			size_t count = decoder->idxend;
			// With compressed instructions enabled, we get the instruction
			// count from the 8-bit instr_count value.
			size_t instr_count = count+1;
			if constexpr (compressed_enabled) {
				instr_count = decoder->idxend - decoder->instr_count;
			} else {
				pc += count * 4;
			}
			counter.increment_counter(instr_count);
			auto* decoder_end = &decoder[count];
			// We want to run 4 instructions at a time, except for
			// the last one, which we will "always" do next
			if constexpr (!compressed_enabled)
			{
				while (decoder + 8 < decoder_end)
				{
					decoder[0].execute(*this);
					decoder[1].execute(*this);
					decoder[2].execute(*this);
					decoder[3].execute(*this);
					decoder[4].execute(*this);
					decoder[5].execute(*this);
					decoder[6].execute(*this);
					decoder[7].execute(*this);
					decoder += 8;
				}
				if (decoder + 4 < decoder_end)
				{
					decoder[0].execute(*this);
					decoder[1].execute(*this);
					decoder[2].execute(*this);
					decoder[3].execute(*this);
					decoder += 4;
				}
			} else { // Conservative compressed version
				while (decoder + 4 < decoder_end)
				{
					// Here we try to avoid re-reading decoder memory
					// after executing instructions, by using locals.
					auto* decoder0 = decoder;
					const auto oplen0 = decoder0->opcode_length;
					auto* decoder1 = decoder0 + oplen0 / 2;
					const auto oplen1 = decoder1->opcode_length;

					decoder0->execute(*this);
					decoder1->execute(*this);

					pc += oplen0 + oplen1;
					decoder = decoder1 + oplen1 / 2;
				}
			}
			// Execute remainder with no PC-dependency
			constexpr int OFF = compressed_enabled ? 2 : 0;
			while (decoder+OFF < decoder_end) {
				// Execute instruction using handler and 32-bit wrapper
				decoder->execute(*this);
				// increment *local* PC
				if constexpr (compressed_enabled) {
					auto length = decoder->opcode_length;
					pc += length;
					decoder += length / 2;
				} else {
					decoder++;
				}
			}
			// Execute last instruction (with updated PC)
			registers().pc = pc;
			decoder->execute(*this);
			// The loop above ended because PC could have changed.
			// Update to real PC by reading from register memory.
			if constexpr (compressed_enabled)
				pc = registers().pc + decoder->opcode_length;
			else
				pc = registers().pc + 4;

			// If we left the execute segment, we have to
			// fall back to slower, precise simulation.
			if (UNLIKELY(!(pc >= current_begin && pc < current_end))) {
				break;
			}

		} while (!counter.overflowed());

		registers().pc = pc;

		// If we are here because the program is still running,
		// but we are outside the decoder cache, use precise simulation
		if (UNLIKELY(!current_exec->is_within(pc) && !counter.overflowed())) {
			// The slowpath only returns if the segment is executable
			// again, or the instruction limit was reached.
			this->next_execute_segment();
			goto restart_simulation;
		}

	} // CPU::simulate

	template<int W>
	void CPU<W>::simulate(uint64_t imax)
	{
#ifndef RISCV_THREADED
		simulate_fastsim(imax);
#else
		simulate_threaded(imax);
#endif
	}

	template struct CPU<4>;
	template struct CPU<8>;
	template struct CPU<16>;
} // riscv
