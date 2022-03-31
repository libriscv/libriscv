#include "machine.hpp"
#include "decoder_cache.hpp"
#include "riscvbase.hpp"
#include "rv32i_instr.hpp"
#include "rv32i.hpp"
#include "rv64i.hpp"
#include "rv128i.hpp"
#ifdef RISCV_FAST_SIMULATOR
#include "fastsim.hpp"
#endif

#define INSTRUCTION_LOGGING(cpu)	\
	if ((cpu).machine().verbose_instructions) { \
		const auto string = isa_type<W>::to_string(cpu, instruction, decode(instruction)) + "\n"; \
		(cpu).machine().print(string.c_str(), string.size()); \
	}

namespace riscv
{
	[[maybe_unused]] static constexpr bool VERBOSE_FASTSIM = false;

	template <int W>
	CPU<W>::CPU(Machine<W>& machine, unsigned cpu_id, const Machine<W>& other)
		: m_machine { machine }, m_cpuid { cpu_id }
	{
		this->m_exec_data  = other.cpu.m_exec_data;
		this->m_exec_begin = other.cpu.m_exec_begin;
		this->m_exec_end   = other.cpu.m_exec_end;

		this->registers() = other.cpu.registers();
#ifdef RISCV_FAST_SIMULATOR
		this->m_qcdata = other.cpu.m_qcdata;
		this->m_fastsim_vector = m_qcdata->data();
#endif
#ifdef RISCV_EXT_ATOMICS
		this->m_atomics = other.cpu.m_atomics;
#endif
	}
	template <int W>
	void CPU<W>::reset()
	{
		this->m_regs = {};
		this->reset_stack_pointer();
		// We can't jump if there's been no ELF loader
		if (!machine().memory.binary().empty()) {
			this->jump(machine().memory.start_address());
		}
		// reset the page cache
		this->m_cache = {};
	}

	template <int W>
	void CPU<W>::init_execute_area(const uint8_t* data, address_t begin, address_t length)
	{
		this->initialize_exec_segs(data - begin, begin, length);
	#ifdef RISCV_INSTR_CACHE
		machine().memory.generate_decoder_cache({}, begin, begin, length);
	#endif
	}

	template <int W> __attribute__((noinline))
	typename CPU<W>::format_t CPU<W>::read_next_instruction_slowpath()
	{
		// Fallback: Read directly from page memory
		const auto pageno = this->pc() >> Page::SHIFT;
		// Page cache
		auto& entry = this->m_cache;
		if (entry.pageno != pageno || entry.page == nullptr) {
			auto e = decltype(m_cache){pageno, &machine().memory.get_exec_pageno(pageno)};
			if (!e.page->attr.exec) {
				trigger_exception(EXECUTION_SPACE_PROTECTION_FAULT, this->pc());
			}
			// delay setting entry until we know it's good!
			entry = e;
		}
		const auto& page = *entry.page;
		const auto offset = this->pc() & (Page::size()-1);
		format_t instruction;

		// Unfortunately, we have to under-align words for C-extension
		// If the C-extension if disabled, we can read whole words.
		union unaligned32 {
			uint32_t to32() const noexcept {
#ifdef RISCV_EXT_COMPRESSED
				return data[0] | (uint32_t)data[1] << 16u;
			}
			uint16_t data[2];
#else
				return data;
			}
			uint32_t data;
#endif
		};

		if (LIKELY(offset <= Page::size()-4)) {
			instruction.whole = ((unaligned32*) (page.data() + offset))->to32();
			return instruction;
		}
		// It's not possible to jump to a misaligned address,
		// so there is necessarily 16-bit left of the page now.
		instruction.whole = *(uint16_t*) (page.data() + offset);

		// If it's a 32-bit instruction at a page border, we need
		// to get the next page, and then read the upper half
		if (UNLIKELY(instruction.is_long()))
		{
			const auto& page = machine().memory.get_exec_pageno(pageno+1);
			instruction.half[1] = *(uint16_t*) page.data();
		}

		return instruction;
	}

	template <int W>
	typename CPU<W>::format_t CPU<W>::read_next_instruction()
	{
		if (LIKELY(this->pc() >= m_exec_begin && this->pc() < m_exec_end)) {
			return format_t { *(uint32_t*) &m_exec_data[this->pc()] };
		}

		return read_next_instruction_slowpath();
	}

	template<int W> __attribute__((hot, no_sanitize("undefined")))
	void CPU<W>::simulate(uint64_t max)
	{
#ifdef RISCV_INSTR_CACHE
		auto* exec_decoder = machine().memory.get_decoder_cache();
		auto* exec_seg_data = this->m_exec_data;
#endif
		// Calculate the instruction limit
		if (max != UINT64_MAX)
			machine().set_max_instructions(machine().instruction_counter() + max);
		else
			machine().set_max_instructions(UINT64_MAX);

		for (; machine().instruction_counter() < machine().max_instructions();
			machine().increment_counter(1)) {

			format_t instruction;
#ifdef RISCV_DEBUG
			this->break_checks();
#endif

# ifdef RISCV_INSTR_CACHE
#  ifndef RISCV_INBOUND_JUMPS_ONLY
		if (LIKELY(this->pc() >= m_exec_begin && this->pc() < m_exec_end)) {
#  endif
			// Instructions may be unaligned with C-extension
			// On amd64 we take the cost, because it's faster
#    if defined(RISCV_EXT_COMPRESSED) && !defined(__x86_64__)
			union Align32 {
				uint16_t data[2];
				operator uint32_t() {
					return data[0] | uint32_t(data[1]) << 16;
				}
			};
			instruction = format_t { *(Align32*) &exec_seg_data[this->pc()] };
#    else
			instruction = format_t { *(uint32_t*) &exec_seg_data[this->pc()] };
#    endif
			// Retrieve handler directly from the instruction handler cache
			auto& cache_entry =
				exec_decoder[this->pc() / DecoderCache<W>::DIVISOR];
		#ifndef RISCV_INSTR_CACHE_PREGEN
			if (UNLIKELY(!DecoderCache<W>::isset(cache_entry))) {
				DecoderCache<W>::convert(this->decode(instruction), cache_entry);
			}
		#endif
		#ifdef RISCV_DEBUG
			INSTRUCTION_LOGGING(*this);
			// Execute instruction
			cache_entry.handler.handler(*this, instruction);
		#else
			// Debugging aid when fast simulator is behaving strangely
			if constexpr (VERBOSE_FASTSIM) {
				verbose_fast_sim(*this, cache_entry.handler, instruction);
			} // Debugging aid
			// Execute instruction
			cache_entry.handler(*this, instruction);
		#endif
#  ifndef RISCV_INBOUND_JUMPS_ONLY
		} else {
			instruction = read_next_instruction_slowpath();
	#ifdef RISCV_DEBUG
			INSTRUCTION_LOGGING(*this);
	#endif
			// decode & execute instruction directly
			this->execute(instruction);
		}
#  endif
# else
			instruction = this->read_next_instruction();
	#ifdef RISCV_DEBUG
			INSTRUCTION_LOGGING(*this);
	#endif
			// decode & execute instruction directly
			this->execute(instruction);
# endif

#ifdef RISCV_DEBUG
			if (UNLIKELY(machine().verbose_registers)) {
				this->register_debug_logging();
			}
#endif
			// increment PC
			if constexpr (compressed_enabled)
				registers().pc += instruction.length();
			else
				registers().pc += 4;
		} // while not stopped

	} // CPU::simulate

	template<int W>
	void CPU<W>::step_one()
	{
		// This will make sure we can do one step while still preserving
		// the max instructions that we had before. If the machine is stopped
		// the old count is not preserved.
		auto old_maxi = machine().max_instructions();
		this->simulate(1);
		if (machine().max_instructions() != 0)
			machine().set_max_instructions(old_maxi);
	}

#ifdef RISCV_FAST_SIMULATOR
	template<int W> __attribute__((hot, no_sanitize("undefined")))
	void CPU<W>::fast_simulator(CPU& cpu, instruction_format instref)
	{
		auto pc = cpu.pc();
		auto fsindex = instref.half[0];
	restart_fastsim:
		const auto& qcvec = cpu.m_fastsim_vector[fsindex];
		//printf("Fast simulation of %zu instructions at 0x%lX\n",
		//	qcvec.data.size(), (long)pc);

	restart_sequence:
		size_t index = 0;
		if constexpr (false && compressed_enabled) {
			address_t base_pc = qcvec.base_pc;
			while (base_pc < pc) {
				const rv32i_instruction instr{ qcvec.data[index].instr };
				base_pc += instr.length();
				index ++;
				//printf("Index %zu with PC 0x%lX / 0x%lX\n", index, (long)base_pc, (long)pc);
			}
		} else {
			index = (pc - qcvec.base_pc) / 4;
		}
		// NOTE: Increment counter based on where we start
		cpu.machine().increment_counter(qcvec.data.size() - index);

		const auto* idata = &qcvec.data[index];
		const auto* iend  = &qcvec.data[qcvec.data.size()];
		for (; idata < iend; idata++) {
			const format_t instruction {idata->instr};
			// Some instructions use PC offsets
			cpu.registers().pc = pc;
			// Debugging aid when fast simulator is behaving strangely
			if constexpr (VERBOSE_FASTSIM) {
				const auto string = isa_type<W>::to_string(cpu, instruction, decode(instruction)) + "\n";
				cpu.machine().print(string.c_str(), string.size());
			}
			// Execute instruction using handler and 32-bit wrapper
			idata->handler(cpu, instruction);
			// increment *local* PC
			if constexpr (compressed_enabled)
				pc += instruction.length();
			else
				pc += 4;
		} // idata

		if (UNLIKELY(cpu.machine().instruction_limit_reached()))
			return;

		// Fast path, but need to check if we are running out of
		// instructions by checking the counter.
		if constexpr (compressed_enabled) {
			// The outer simulator will miscalculate the size of the instruction
			cpu.registers().pc += qcvec.incrementor;
			//pc += qcvec.incrementor;
		} else {
			pc = cpu.pc() + 4;
			if (pc >= qcvec.base_pc && pc < qcvec.end_pc) {
				goto restart_sequence;
			}
		}

		// Check if the next instruction handler is this very function.
		// If so, restart procedure with updated fastsim index.
		if constexpr (false) {
			auto* exec_decoder = cpu.machine().memory.get_decoder_cache();
			auto* exec_seg_data = cpu.m_exec_data;
			// Unfortunately, it is slower than simply returning.
			auto& cache_entry = exec_decoder[pc / DecoderCache<W>::DIVISOR];
			if (cache_entry.handler == &fast_simulator) {
				fsindex = *(uint16_t *) &exec_seg_data[pc];
				//printf("Restarting at 0x%lX with index %u / %zu\n",
				//	(long)pc, fsindex, cpu.m_qcdata->size());
				goto restart_fastsim;
			}
		}

		// Compensate for the outer simulator incrementing the
		// instruction counter.
		cpu.machine().increment_counter(-1);
	} // CPU::fast_simulator
#endif

	template<int W> __attribute__((cold))
	void CPU<W>::trigger_exception(int intr, address_t data)
	{
		switch (intr)
		{
		case ILLEGAL_OPCODE:
			throw MachineException(ILLEGAL_OPCODE,
					"Illegal opcode executed", data);
		case ILLEGAL_OPERATION:
			throw MachineException(ILLEGAL_OPERATION,
					"Illegal operation during instruction decoding", data);
		case PROTECTION_FAULT:
			throw MachineException(PROTECTION_FAULT,
					"Protection fault", data);
		case EXECUTION_SPACE_PROTECTION_FAULT:
			throw MachineException(EXECUTION_SPACE_PROTECTION_FAULT,
					"Execution space protection fault", data);
		case MISALIGNED_INSTRUCTION:
			// NOTE: only check for this when jumping or branching
			throw MachineException(MISALIGNED_INSTRUCTION,
					"Misaligned instruction executed", data);
		case UNIMPLEMENTED_INSTRUCTION:
			throw MachineException(UNIMPLEMENTED_INSTRUCTION,
					"Unimplemented instruction executed", data);
		case DEADLOCK_REACHED:
			throw MachineException(DEADLOCK_REACHED,
					"Atomics deadlock reached", data);
		default:
			throw MachineException(UNKNOWN_EXCEPTION,
					"Unknown exception", intr);
		}
	}

	template <int W> __attribute__((cold))
	std::string CPU<W>::to_string(format_t format, const instruction_t& instr) const
	{
		if constexpr (W == 4)
			return RV32I::to_string(*this, format, instr);
		else if constexpr (W == 8)
			return RV64I::to_string(*this, format, instr);
		else if constexpr (W == 16)
			return RV128I::to_string(*this, format, instr);
		return "Unknown architecture";
	}

	template <int W> __attribute__((cold))
	std::string Registers<W>::flp_to_string() const
	{
		char buffer[800];
		int  len = 0;
		for (int i = 0; i < 32; i++) {
			auto& src = this->getfl(i);
			const char T = (src.i32[1] == -1) ? 'S' : 'D';
			double val = (src.i32[1] == -1) ? src.f32[0] : src.f64;
			len += snprintf(buffer+len, sizeof(buffer) - len,
					"[%s\t%c%+.2f] ", RISCV::flpname(i), T, val);
			if (i % 5 == 4) {
				len += snprintf(buffer+len, sizeof(buffer)-len, "\n");
			}
		}
		return std::string(buffer, len);
	}

	template struct CPU<4>;
	template struct Registers<4>;
	template struct CPU<8>;
	template struct Registers<8>;
	template struct CPU<16>;
	template struct Registers<16>;
}
