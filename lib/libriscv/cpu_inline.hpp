
template <int W>
CPU<W>::CPU(Machine<W>& machine)
	: m_machine { machine }
{
}

template<int W>
void CPU<W>::jump(const address_t dst)
{
	this->registers().pc = dst;
	// it's possible to jump to a misaligned address
	if (UNLIKELY(this->registers().pc & 0x1)) {
		this->trigger_interrupt(MISALIGNED_INSTRUCTION);
	}
}

template<int W>
void CPU<W>::trigger_interrupt(interrupt_t intr)
{
	m_data.interrupt_queue.push_back(intr);
}

#ifdef RISCV_DEBUG

template <int W>
inline void CPU<W>::breakpoint(address_t addr, breakpoint_t func) {
	this->m_breakpoints[addr] = func;
}

template <int W>
inline void CPU<W>::default_pausepoint(CPU& cpu)
{
	cpu.machine().print_and_pause();
}

#endif
