
template <int W>
inline CPU<W>::CPU(Machine<W>& machine)
	: m_machine { machine }
{
}
template <int W>
inline void CPU<W>::reset_stack_pointer() noexcept
{
	// initial stack location
	this->reg(RISCV::REG_SP) = machine().memory.stack_initial();
}

template<int W> constexpr
inline void CPU<W>::jump(const address_t dst)
{
	this->registers().pc = dst;
	// it's possible to jump to a misaligned address
	if constexpr (!compressed_enabled) {
		if (UNLIKELY(this->registers().pc & 0x3)) {
			this->trigger_exception(MISALIGNED_INSTRUCTION, registers().pc);
		}
	} else {
		if (UNLIKELY(this->registers().pc & 0x1)) {
			this->trigger_exception(MISALIGNED_INSTRUCTION, registers().pc);
		}
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
