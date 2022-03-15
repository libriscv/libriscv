
template <int W>
inline CPU<W>::CPU(Machine<W>& machine, unsigned cpu_id)
	: m_machine { machine }, m_cpuid { cpu_id }
{
}
template <int W>
inline void CPU<W>::reset_stack_pointer() noexcept
{
	// initial stack location
	this->reg(2) = machine().memory.stack_initial();
}

template<int W>
inline void CPU<W>::jump(const address_t dst)
{
#ifdef RISCV_INBOUND_JUMPS_ONLY
	if (UNLIKELY(dst < m_exec_begin || dst >= m_exec_end)) {
		trigger_exception(EXECUTION_SPACE_PROTECTION_FAULT, dst);
	}
#endif
	// it's possible to jump to a misaligned address
	if constexpr (!compressed_enabled) {
		if (UNLIKELY(dst & 0x3)) {
			trigger_exception(MISALIGNED_INSTRUCTION, dst);
		}
	} else {
		if (UNLIKELY(dst & 0x1)) {
			trigger_exception(MISALIGNED_INSTRUCTION, dst);
		}
	}
	this->registers().pc = dst;
}

template<int W>
inline void CPU<W>::aligned_jump(const address_t dst)
{
#ifdef RISCV_INBOUND_JUMPS_ONLY
	if (UNLIKELY(dst < m_exec_begin || dst >= m_exec_end)) {
		trigger_exception(EXECUTION_SPACE_PROTECTION_FAULT, dst);
	}
#endif
	this->registers().pc = dst;
}

template<int W>
inline void CPU<W>::increment_pc(int delta)
{
	registers().pc += delta;
}

template <int W>
inline void CPU<W>::initialize_exec_segs(const uint8_t* data, address_t begin, address_t length)
{
	m_exec_data = data; m_exec_begin = begin; m_exec_end = begin + length;
}

#ifdef RISCV_DEBUG

template <int W>
inline void CPU<W>::breakpoint(address_t addr, breakpoint_t func) {
	if (func)
		this->m_breakpoints[addr] = func;
	else
		this->m_breakpoints.erase(addr);
}

template <int W>
inline void CPU<W>::default_pausepoint(CPU& cpu)
{
	cpu.machine().print_and_pause();
}

#endif
