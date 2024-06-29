#pragma once
#include "common.hpp"
#include "page.hpp"
#include "registers.hpp"
#ifdef RISCV_EXT_ATOMICS
#include "rva.hpp"
#endif
#include <vector>

namespace riscv
{
	template<int W> struct Machine;
	template<int W> struct DecodedExecuteSegment;

	template<int W>
	struct alignas(64) CPU
	{
		using address_t = address_type<W>;     // one unsigned memory address
		using format_t  = instruction_format;  // machine instruction format
		using breakpoint_t = std::function<void(CPU<W>&)>;
		using execute_fault_t = void(*)(CPU<W>&, const Page&);
		using override_execute_segment_t = DecodedExecuteSegment<W>&(*)(CPU<W>&);
		using instruction_t = Instruction<W>;

		// Dispatch modes (determined at configure-time):
		// 1. Switch-case: Uses a big switch-case for bytecodes. Works and runs everywhere.
		// 2. Threaded: Uses computed gotos to jump around at a faster speed, but
		//    is only supported on GCC and Clang. Very fast dispatch.
		// 3. TCO: Uses musttail to jump around at the fastest speed, but
		//    is only supported on Clang. Very fast dispatch.
		/// Executes RISC-V code using the configure-time dispatch mode.
		/// @param pc The starting address
		/// @param icounter The instruction counter start value (usually 0)
		/// @param maxcounter The instruction limit value (usually several millions)
		/// @return Returns true if the machine stopped normally, otherwise an execution timeout happened.
		bool simulate(address_t pc, uint64_t icounter, uint64_t maxcounter);

		// Step precisely one instruction forward from current PC.
		void step_one();

		// Executes one instruction at a time, and can stop at
		// any instruction. Can be used for debugging.
		void simulate_precise();

		/// @brief  Get the current PC
		/// @return The current PC address
		address_t pc() const noexcept { return registers().pc; }
		void increment_pc(int delta) noexcept;
		void jump(address_t);
		void aligned_jump(address_t) noexcept;

		/// @brief Retrieve current register state
		/// @return Current register state
		RISCV_ALWAYS_INLINE auto& registers() noexcept { return this->m_regs; }
		/// @brief Retrieve current register state
		/// @return Current register state
		RISCV_ALWAYS_INLINE const auto& registers() const noexcept { return this->m_regs; }

		int cpu_id() const noexcept { return m_cpuid; }

		auto& reg(uint32_t idx) noexcept { return registers().get(idx); }
		const auto& reg(uint32_t idx) const noexcept { return registers().get(idx); }
		auto& cireg(uint16_t idx) noexcept { return registers().get(idx + 0x8); }
		const auto& cireg(uint16_t idx) const noexcept { return registers().get(idx + 0x8); }
		auto& ciflp(uint16_t idx) noexcept { return registers().getfl(idx + 0x8); }

		Machine<W>& machine() noexcept;
		const Machine<W>& machine() const noexcept;

		Memory<W>& memory() noexcept;
		const Memory<W>& memory() const noexcept;

#ifdef RISCV_EXT_ATOMICS
		auto& atomics() noexcept { return memory().atomics(); }
		const auto& atomics() const noexcept { return memory().atomics(); }
		template <typename Type>
		void amo(format_t, Type(*op)(CPU&, Type&, uint32_t));
#endif

		[[noreturn]]
		static void trigger_exception(int which, address_t = 0) RISCV_COLD_PATH();

		// Directly execute an instruction (given bits)
		void execute(format_t);
		// Read the next instruction bits
		format_t read_next_instruction() const;
		// Internal preempt() implementation that executes and restores old registers
		address_t preempt_internal(Registers<W>& old_regs, bool store_regs, address_t pc, uint64_t max_instr);

		// Pretty print instructions
		std::string to_string(format_t format) const;
		std::string to_string(format_t format, const instruction_t &instr) const;

		/// @brief Pretty-print the current instruction
		/// @return Returns a formatted string of the current instruction
		std::string current_instruction_to_string() const;

		format_t read_next_instruction_slowpath() const RISCV_COLD_PATH();
		static const instruction_t& decode(format_t);
		// Convert a RISC-V instruction into a fast bytecode
		static size_t computed_index_for(format_t bits) noexcept;

		/// @brief Serializes the current CPU state to a vector
		/// @param vec The vector to serialize into
		void serialize_to(std::vector<uint8_t>& vec) const;

		/// @brief Returns the CPU to a previously stored state. Used by Machine::deserialize_from.
		/// @param vec The vector to deserialize from
		/// @param sm The serialized machine header to get metadata from
		void deserialize_from(const std::vector<uint8_t>& vec, const SerializedMachine<W>& sm);

		// Binary translation functions
		int  load_translation(const MachineOptions<W>&, std::string* filename, DecodedExecuteSegment<W>&) const;
		void try_translate(const MachineOptions<W>&, const std::string&, DecodedExecuteSegment<W>&, address_t pc, address_t endpc) const;

		void reset();
		void reset_stack_pointer() noexcept;

		CPU(Machine<W>&, unsigned cpu_id);
		CPU(Machine<W>&, unsigned cpu_id, const Machine<W>& other); // Fork

		DecodedExecuteSegment<W>& init_execute_area(const void* data, address_t begin, address_t length);
		void set_execute_segment(DecodedExecuteSegment<W>& seg) noexcept { m_exec = &seg; }
		auto& current_execute_segment() noexcept { return *m_exec; }
		auto& current_execute_segment() const noexcept { return *m_exec; }
		struct NextExecuteReturn {
			DecodedExecuteSegment<W>* exec;
			address_t pc;
		};
		NextExecuteReturn next_execute_segment(address_t pc);
		static std::shared_ptr<DecodedExecuteSegment<W>>& empty_execute_segment() noexcept;
		bool is_executable(address_t addr) const noexcept;

		// Override the function that gets called when the CPU
		// throws an execute space protection fault.
		void set_fault_handler(execute_fault_t func) noexcept { m_fault = func; }

		// Override how to produce the next active execute segment
		void set_override_new_execute_segment(override_execute_segment_t func) noexcept { m_override_exec = func; }

		// Override how to handle unknown instructions, so that you may implement your own
		static inline std::function<const instruction_t& (format_t)> on_unimplemented_instruction;
		// Retrieve default handler for unimplemented instructions (can be returned in on_unimplemented_instruction)
		static const instruction_t& get_unimplemented_instruction() noexcept;

		// Set current exception
		void set_current_exception(std::exception_ptr&& ptr) noexcept { m_current_exception = std::move(ptr); }
		void clear_current_exception() noexcept { m_current_exception = nullptr; }
		bool has_current_exception() const noexcept { return m_current_exception != nullptr; }
		auto& current_exception() const noexcept { return m_current_exception; }

	private:
		Registers<W> m_regs;
		Machine<W>&  m_machine;

		// ELF programs linear .text segment (initialized as empty segment)
		DecodedExecuteSegment<W>* m_exec;

		// Page cache for execution on virtual memory
		mutable CachedPage<W, const Page> m_cache;

		const unsigned m_cpuid;

		// The current exception (used by eg. TCC which doesn't create unwinding tables)
		std::exception_ptr m_current_exception = nullptr;

		// The default execute fault simply triggers the exception
		execute_fault_t m_fault = [] (auto& cpu, auto&) {
			trigger_exception(EXECUTION_SPACE_PROTECTION_FAULT, cpu.pc());
		};

		// The default execute override returns no new execute segment
		override_execute_segment_t m_override_exec = [] (auto&) -> DecodedExecuteSegment<W>& {
			return *empty_execute_segment();
		};

#ifdef RISCV_BINARY_TRANSLATION
		std::vector<TransMapping<W>> emit(std::string& code, const TransInfo<W>&) const;
		void activate_dylib(const MachineOptions<W>&, DecodedExecuteSegment<W>&, void*) const RISCV_INTERNAL;
		bool initialize_translated_segment(DecodedExecuteSegment<W>&, void*) const RISCV_INTERNAL;
#endif
		static_assert((W == 4 || W == 8 || W == 16), "Must be either 32-bit, 64-bit or 128-bit ISA");
	};

#include "cpu_inline.hpp"
} // riscv
