#include <cstdint>

namespace riscv
{
    template <int W> struct Machine;

	// In fastsim mode the instruction counter becomes a register
	// the function, and we only update m_counter in Machine on exit
	// When binary translation is enabled we cannot do this optimization.
	template <int W>
	struct InstrCounter {
		InstrCounter(Machine<W>& m) : machine(m), m_counter{m.instruction_counter()} {}
		~InstrCounter() {
			if constexpr (!binary_translation_enabled)
				machine.set_instruction_counter(m_counter);
		}

		void apply() {
			machine.set_instruction_counter(m_counter);
		}
		uint64_t value() const noexcept {
			if constexpr (binary_translation_enabled)
				return machine.instruction_counter();
			else
				return m_counter; }
		void set_counter(uint64_t value) {
			m_counter = value;
		}
		void increment_counter(uint64_t cnt) {
			if constexpr (binary_translation_enabled)
				machine.increment_counter(cnt);
			else
				m_counter += cnt;
		}
		bool overflowed() const noexcept {
			if constexpr (binary_translation_enabled)
				return machine.stopped();
			else
				return m_counter >= machine.max_instructions();
		}
	private:
		Machine<W>& machine;
		uint64_t m_counter;
	};
} // riscv
