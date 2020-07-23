
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

template <int W> __attribute__((hot))
inline void CPU<W>::change_page(address_t pageno)
{
#ifdef RISCV_PAGE_CACHE
	for (const auto& cache : m_page_cache) {
		if (cache.pageno == pageno) {
			m_current_page = cache;
			// NOTE: this lowers instruction cache pressure
			goto riscv_validate_current_page;
		}
	}
#endif
	m_current_page.pageno = pageno;
	m_current_page.page = &machine().memory.get_exec_pageno(pageno);
#ifdef RISCV_PAGE_CACHE
	// Cache it
	m_page_cache[m_cache_iterator % m_page_cache.size()] = m_current_page;
	m_cache_iterator ++;
riscv_validate_current_page:
#endif
	// Execute traps enables trapping on execute and enables a novel way of
	// invoking system calls as regular functions by calling an address.
	if constexpr (execute_traps_enabled) {
		// If this trap immediately returns to the caller then by design the
		// caller will avoid faulting on a page with no execute permission.
		this->check_page();
	}
	// Verify execute permission
	if (UNLIKELY(!m_current_page.page->attr.exec)) {
		this->trigger_exception(EXECUTION_SPACE_PROTECTION_FAULT,
			pageno * Page::size());
	}
#ifdef RISCV_INSTR_CACHE
	if (UNLIKELY(m_current_page.page->decoder_cache() == nullptr)) {
		m_current_page.page->create_decoder_cache();
	}
#endif
}

template <int W> __attribute__((hot))
inline void CPU<W>::check_page()
{
	const auto& cp = m_current_page;
	if (UNLIKELY(cp.page->has_trap())) {
		const address_t old_pageno = cp.pageno;
		cp.page->trap(this->pc() - (cp.pageno << Page::SHIFT), TRAP_EXEC, cp.pageno);
		const address_t new_pageno = this->pc() >> Page::SHIFT;
		if (old_pageno != new_pageno) {
			this->change_page(new_pageno);
		}
	}
}

template<int W> constexpr __attribute__((hot))
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
