
template <int W>
inline CPU<W>::CPU(Machine<W>& machine)
	: m_machine { machine }
{
}

template <int W>
inline void CPU<W>::change_page(address_t this_page)
{
#ifdef RISCV_PAGE_CACHE
	for (const auto& cache : m_page_cache) {
		if (cache.address == this_page) {
			m_current_page = cache;
			return;
		}
	}
#endif
	m_current_page.address = this_page;
	m_current_page.page = &machine().memory.create_page(this_page >> Page::SHIFT);
#ifdef RISCV_PAGE_CACHE
	// cache it
	m_page_cache[m_cache_iterator] = m_current_page;
	m_cache_iterator = (m_cache_iterator + 1) % m_page_cache.size();
#endif
	// execute traps have priority over execute permission
if constexpr (execute_traps_enabled) {
	if (UNLIKELY(m_current_page.page->has_trap())) {
		m_current_page.page->trap(this_page, TRAP_EXEC, 0x0);
		return;
	}
}
	// verify execute permission
	if (UNLIKELY(!m_current_page.page->attr.exec)) {
		this->trigger_exception(EXECUTION_SPACE_PROTECTION_FAULT);
	}
}

template<int W> constexpr
inline void CPU<W>::jump(const address_t dst)
{
	this->registers().pc = dst;
	// it's possible to jump to a misaligned address
	if constexpr (!compressed_enabled) {
		if (UNLIKELY(this->registers().pc & 0x3)) {
			this->trigger_exception(MISALIGNED_INSTRUCTION);
		}
	} else {
		if (UNLIKELY(this->registers().pc & 0x1)) {
			this->trigger_exception(MISALIGNED_INSTRUCTION);
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
