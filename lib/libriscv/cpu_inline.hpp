
template <int W>
inline CPU<W>::CPU(Machine<W>& machine, unsigned cpu_id)
	: m_machine { machine }, m_exec(&empty_execute_segment()), m_cpuid { cpu_id }
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
	this->registers().pc = dst;
}

template<int W>
inline void CPU<W>::increment_pc(int delta)
{
	registers().pc += delta;
}
