#include <cstdint>

namespace riscv
{
    template <int W> struct Machine;

	// In fastsim mode the instruction counter becomes a register
	// the function, and we only update m_counter in Machine on exit
	// When binary translation is enabled we cannot do this optimization.
	template <int W>
	struct InstrCounter {
		InstrCounter(Machine<W>& m)
		  : machine(m),
		    m_counter(m.instruction_counter()),
			m_max(m.max_instructions())
		{}
		~InstrCounter() {
			machine.set_instruction_counter(m_counter);
		}

		void apply() {
			machine.set_instruction_counter(m_counter);
			machine.set_max_instructions(m_max);
		}
		void apply_counter() {
			machine.set_instruction_counter(m_counter);
		}
		void retrieve() {
			m_counter = machine.instruction_counter();
			m_max     = machine.max_instructions();
		}
		void retrieve_max_counter() {
			m_max     = machine.max_instructions();
		}
		uint64_t value() const noexcept {
			return m_counter;
		}
		uint64_t max() const noexcept {
			return m_max;
		}
		void stop() noexcept {
			m_max = 0; // This stops the machine
		}
		void set_counter(uint64_t value) {
			m_counter = value;
		}
		void increment_counter(uint64_t cnt) {
			m_counter += cnt;
		}
		bool overflowed() const noexcept {
			return m_counter >= m_max;
		}
	private:
		Machine<W>& machine;
		uint64_t m_counter;
		uint64_t m_max;
	};
} // riscv
