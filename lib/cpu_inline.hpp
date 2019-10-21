#include "cpu.hpp"

template<int W>
inline void CPU<W>::reset()
{
	m_data = {};
	m_data.pc = machine().memory.start_address();
}

template<int W>
inline void CPU<W>::simulate()
{
	this->execute();
}

template<int W>
inline void CPU<W>::trigger_interrupt(interrupt_t intr)
{
	m_data.interrupt_queue.push_back(intr);
}

template<int W>
inline void CPU<W>::handle_interrupts()
{
	if (m_data.interrupt_master_enable && !m_data.interrupt_queue.empty())
	{
		for (auto intr : m_data.interrupt_queue) {
			this->execute_interrupt(intr);
		}
		m_data.interrupt_queue.clear();
	}
}

template<int W>
inline void CPU<W>::execute_interrupt(interrupt_t intr)
{
	// TODO: replace with callback system
	switch (intr)
	{
	case ILLEGAL_OPCODE:
		throw std::runtime_error("Illegal opcode executed");
	case MISALIGNED_INSTRUCTION:
		// NOTE: only check for this when jumping or branching
		throw std::runtime_error("Misaligned instruction executed");
	case UNIMPLEMENTED_INSTRUCTION:
		throw std::runtime_error("Unimplemented instruction executed");
	default:
		throw std::runtime_error("Interrupt not implemented: " + std::to_string(intr));
	}
}
