#pragma once

template <int W>
template <typename T> inline
T Memory<W>::read(address_t address)
{
	const auto& page = get_readable_page(address);

#ifdef RISCV_PAGE_TRAPS_ENABLED
	if constexpr (memory_traps_enabled) {
		if (UNLIKELY(page.has_trap())) {
			return page.trap(address & (Page::size()-1), sizeof(T) | TRAP_READ, 0);
		}
	}
#endif
	return page.template aligned_read<T>(address & (Page::size()-1));
}

template <int W>
template <typename T> inline
void Memory<W>::write(address_t address, T value)
{
	auto& page = get_writable_page(address);

#ifdef RISCV_PAGE_TRAPS_ENABLED
	if constexpr (memory_traps_enabled) {
		if (UNLIKELY(page.has_trap())) {
			page.trap(address & (Page::size()-1), sizeof(T) | TRAP_WRITE, value);
			return;
		}
	}
#endif
	page.template aligned_write<T>(address & (Page::size()-1), value);
}

template <int W>
inline const Page& Memory<W>::get_page(const address_t address) const noexcept
{
	const auto page = page_number(address);
	return get_pageno(page);
}

template <int W>
inline const Page& Memory<W>::get_exec_pageno(const address_t page) const
{
	auto it = m_pages.find(page);
	if (LIKELY(it != m_pages.end())) {
		return it->second;
	}
	machine().cpu.trigger_exception(EXECUTION_SPACE_PROTECTION_FAULT);
	__builtin_unreachable();
}

template <int W>
inline const Page& Memory<W>::get_pageno(const address_t page) const noexcept
{
	auto it = m_pages.find(page);
	if (it != m_pages.end()) {
		return it->second;
	}
	// uninitialized memory is all zeroes on this system
	return Page::cow_page();
}

template <int W> inline void
Memory<W>::invalidate_page(address_t pageno, Page& page)
{
	// it's only possible to a have CoW read-only page
	if (m_current_rd_page == pageno) {
		m_current_rd_ptr = &page;
	}
}

template <int W>
template <typename... Args> inline
Page& Memory<W>::allocate_page(const size_t page, Args&&... args)
{
	const auto& it = pages().try_emplace(page, std::forward<Args> (args)...);
	// if this page was read-cached, invalidate it
	this->invalidate_page(page, it.first->second);
	// return new page
	return it.first->second;
}

template <int W>
size_t Memory<W>::nonshared_pages_active() const noexcept
{
	return std::accumulate(m_pages.begin(), m_pages.end(),
				0, [] (int value, const auto& it) {
					return value + (!it.second.attr.non_owning ? 1 : 0);
				});
}

template <int W>
void Memory<W>::trap(address_t page_addr, mmio_cb_t callback)
{
#ifdef RISCV_PAGE_TRAPS_ENABLED
	auto& page = create_page(page_number(page_addr));
	page.set_trap(callback);
#else
	(void) page_addr;
	(void) callback;
#endif
}

template <int W>
address_type<W> Memory<W>::resolve_address(const char* name) const
{
#ifndef RISCV_DISABLE_SYM_LOOKUP
	const auto& it = sym_lookup.find(name);
	if (it != sym_lookup.end()) return it->second;
#endif

	auto* sym = resolve_symbol(name);
	address_t addr = (sym) ? sym->st_value : 0x0;
#ifndef RISCV_DISABLE_SYM_LOOKUP
	sym_lookup.emplace(strdup(name), addr);
#endif
	return addr;
}

template <int W>
address_type<W> Memory<W>::resolve_section(const char* name) const
{
	auto* shdr = this->section_by_name(name);
	if (shdr) return shdr->sh_addr;
	return 0x0;
}

template <int W>
address_type<W> Memory<W>::exit_address() const noexcept
{
	return this->m_exit_address;
}

template <int W>
void Memory<W>::set_exit_address(address_t addr)
{
	this->m_exit_address = addr;
}
