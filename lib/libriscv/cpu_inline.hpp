
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
	// NOTE: if the stack is very low, some stack pointer value could
	// become 0x0 which could alter the behavior of the program,
	// even though the address might be legitimate. To solve this, we move
	// the stack at that time to a safer location.
	if (this->reg(RISCV::REG_SP) < 0x100000) {
		this->reg(RISCV::REG_SP) = 0x40000000;
	}
}

template <int W>
inline void CPU<W>::change_page(int pageno)
{
#ifdef RISCV_PAGE_CACHE
	for (const auto& cache : m_page_cache) {
		if (cache.pageno == pageno) {
			m_current_page = cache;
			if constexpr (execute_traps_enabled) {
				this->check_page(m_current_page);
			}
			return;
		}
	}
#endif
	m_current_page.pageno = pageno;
	m_current_page.page = &machine().memory.get_exec_pageno(pageno);
#ifdef RISCV_PAGE_CACHE
	// cache it
	m_page_cache[m_cache_iterator] = m_current_page;
	m_cache_iterator = (m_cache_iterator + 1) % m_page_cache.size();
#endif
if constexpr (execute_traps_enabled) {
	this->check_page(m_current_page);
}
	// verify execute permission
	if (UNLIKELY(!m_current_page.page->attr.exec)) {
		this->trigger_exception(EXECUTION_SPACE_PROTECTION_FAULT);
	}
#ifdef RISCV_INSTR_CACHE
	if (UNLIKELY(m_current_page.page->decoder_cache() == nullptr)) {
		m_current_page.page->create_decoder_cache();
	}
#endif
}

template <int W>
inline void CPU<W>::check_page(CachedPage& cp)
{
	if (UNLIKELY(cp.page->has_trap())) {
		cp.page->trap(this->pc() - (cp.pageno << Page::SHIFT), TRAP_EXEC, cp.pageno);
		const int new_pageno = this->pc() >> Page::SHIFT;
		if (cp.pageno != new_pageno) {
			this->change_page(new_pageno);
		}
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
