#pragma once
#include "common.hpp"
#include "page.hpp"
#include "registers.hpp"
#include "supervisor.hpp"
#ifdef RISCV_EXT_ATOMICS
#include "rva.hpp"
#endif
#include <vector>

namespace riscv
{
	template<int W> struct Machine;
	template<int W> struct DecodedExecuteSegment;

	template<int W>
	struct CPU
	{
		using address_t = address_type<W>;     // one unsigned memory address
		using format_t  = instruction_format;  // machine instruction format
		using breakpoint_t = std::function<void(CPU<W>&)>;
		using execute_fault_t = void(*)(CPU<W>&, const Page&);
		using override_execute_segment_t = DecodedExecuteSegment<W>*(*)(CPU<W>&);
		using instruction_t = Instruction<W>;

		void simulate(uint64_t);
		void simulate_precise(uint64_t);
		void step_one();
		void reset();
		void reset_stack_pointer() noexcept;

		address_t pc() const noexcept { return registers().pc; }
		void increment_pc(int delta);
		void jump(address_t);
		void aligned_jump(address_t);

		auto& registers() { return this->m_regs; }
		const auto& registers() const { return this->m_regs; }
		int cpu_id() const noexcept { return m_cpuid; }

		auto& reg(uint32_t idx) { return registers().get(idx); }
		const auto& reg(uint32_t idx) const { return registers().get(idx); }
		auto& cireg(uint16_t idx) { return registers().get(idx + 0x8); }
		const auto& cireg(uint16_t idx) const { return registers().get(idx + 0x8); }
		auto& ciflp(uint16_t idx) { return registers().getfl(idx + 0x8); }

		auto& machine() noexcept { return this->m_machine; }
		const auto& machine() const noexcept { return this->m_machine; }

#ifdef RISCV_SUPERVISOR_MODE
		auto& super() {
			if (m_super == nullptr) m_super.reset(new Supervisor<W>);
			return *this->m_super; }
		const auto& super() const {
			if (m_super == nullptr) m_super.reset(new Supervisor<W>);
			return *this->m_super; }
#endif

#ifdef RISCV_EXT_ATOMICS
		auto& atomics() noexcept { return this->m_atomics; }
		const auto& atomics() const noexcept { return this->m_atomics; }
		template <typename Type>
		void amo(format_t, Type(*op)(CPU&, Type&, uint32_t));
#endif
		__attribute__((noreturn))
		static void trigger_exception(int, address_t = 0) COLD_PATH();

		void execute(format_t);
		format_t read_next_instruction() const;
		format_t read_next_instruction_slowpath() const COLD_PATH();
		static const instruction_t& decode(format_t);
		instruction_t decode_rewrite(address_t pc, format_t &);
		std::string to_string(format_t format) const;
		std::string to_string(format_t format, const instruction_t& instr) const;
		std::string current_instruction_to_string() const;

		// Serializes all the machine state + a tiny header to @vec
		void serialize_to(std::vector<uint8_t>& vec) const;
		// Returns the machine to a previously stored state
		void deserialize_from(const std::vector<uint8_t>&, const SerializedMachine<W>&);

		// Binary translation functions
		int  load_translation(const MachineOptions<W>&, std::string* filename) const;
		void try_translate(const MachineOptions<W>&, const std::string&, address_t pc, std::vector<TransInstr<W>>) const;

		CPU(Machine<W>&, unsigned cpu_id);
		CPU(Machine<W>&, unsigned cpu_id, const Machine<W>& other); // Fork
		void init_execute_area(const void* data, address_t begin, address_t length);
		void set_execute_segment(DecodedExecuteSegment<W>* seg) { m_exec = seg; }
		auto* current_execute_segment() const noexcept { return m_exec; }
		bool is_executable(address_t addr) const noexcept;

		// Override the function that gets called when the CPU
		// throws an execute space protection fault.
		void set_fault_handler(execute_fault_t func) { m_fault = func; }

		// Override how to produce the next active execute segment
		void set_override_new_execute_segment(override_execute_segment_t func) { m_override_exec = func; }

	private:
		Registers<W> m_regs;
		Machine<W>&  m_machine;

#ifdef RISCV_SUPERVISOR_MODE
		mutable std::unique_ptr<Supervisor<W>> m_super = nullptr;
#endif

		void next_execute_segment();
		void emit(std::string& code, const std::string& symb, TransInstr<W>* blk, const TransInfo<W>&) const;

		// ELF programs linear .text segment
		DecodedExecuteSegment<W>* m_exec = nullptr;

		// Page cache for execution on virtual memory
		mutable CachedPage<W, const Page> m_cache;

		const unsigned m_cpuid;

		// The default execute fault simply triggers the exception
		execute_fault_t m_fault = [] (auto& cpu, auto&) {
			trigger_exception(EXECUTION_SPACE_PROTECTION_FAULT, cpu.pc());
		};

		// The default execute override returns no new execute segment
		override_execute_segment_t m_override_exec = [] (auto&) {
			return (DecodedExecuteSegment<W> *)nullptr;
		};

#ifdef RISCV_EXT_ATOMICS
		AtomicMemory<W> m_atomics;
#endif
		void activate_dylib(void*) const RISCV_INTERNAL;
		static_assert((W == 4 || W == 8 || W == 16), "Must be either 32-bit, 64-bit or 128-bit ISA");
	};

#include "cpu_inline.hpp"
} // riscv
