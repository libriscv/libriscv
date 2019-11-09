
template <int W>
inline CPU<W>::CPU(Machine<W>& machine)
	: m_machine { machine }
{
}

template<int W>
inline void CPU<W>::jump(const address_t dst)
{
	this->registers().pc = dst;
	// it's possible to jump to a misaligned address
	if (UNLIKELY(this->registers().pc & 0x1)) {
		this->trigger_exception(MISALIGNED_INSTRUCTION);
	}
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
