
template <int W>
inline CPU<W>::CPU(Machine<W>& machine)
	: m_machine { machine }
{
}

template <int W>
inline void CPU<W>::change_page(address_t this_page)
{
	m_current_page = this_page;
	m_page_pointer = &machine().memory.get_pageno(this_page);
	// verify execute permission
	if (UNLIKELY(!m_page_pointer->attr.exec)) {
		this->trigger_exception(EXECUTION_SPACE_PROTECTION_FAULT);
	}
}

template<int W> constexpr
inline void CPU<W>::jump(const address_t dst)
{
	this->registers().pc = dst;
	// it's possible to jump to a misaligned address
	if (UNLIKELY(this->registers().pc & 0x1)) {
		this->trigger_exception(MISALIGNED_INSTRUCTION);
	}
#ifndef RISCV_DEBUG
	const address_t this_page = dst >> Page::SHIFT;
	if (UNLIKELY(this_page != this->m_current_page)) {
		this->change_page(this_page);
	}
	this->m_page_offset = dst & (Page::size()-1);
#endif
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
