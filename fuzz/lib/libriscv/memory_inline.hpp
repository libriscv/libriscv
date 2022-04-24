#pragma once

template <int W>
template <typename T> inline
T Memory<W>::read(address_t address)
{
#ifdef RISCV_FLAT_MEMORY
	if (address+sizeof(T) < m_memsize)
		return *(T *)&m_memdata[address];
	protection_fault(address);
#else
	const auto& pagedata = cached_readable_page(address, sizeof(T));
	return pagedata.template aligned_read<T>(address & (Page::size()-1));
#endif
}

template <int W>
template <typename T> inline
T& Memory<W>::writable_read(address_t address)
{
#ifdef RISCV_FLAT_MEMORY
	if (address+sizeof(T) < m_memsize)
		return *(T *)&m_memdata[address];
	protection_fault(address);
#else
	auto& pagedata = cached_writable_page(address);
	return pagedata.template aligned_read<T>(address & (Page::size()-1));
#endif
}

template <int W>
template <typename T> inline
void Memory<W>::write(address_t address, T value)
{
#ifdef RISCV_FLAT_MEMORY
	if (address+sizeof(T) < m_memsize) {
		*(T *)&m_memdata[address] = value;
		return;
	}
	protection_fault(address);
#else
	const auto pageno = page_number(address);
	auto& entry = m_wr_cache;
	if (entry.pageno == pageno) {
		entry.page->template aligned_write<T>(address & (Page::size()-1), value);
		return;
	}

	auto& page = create_writable_pageno(pageno);
	if (LIKELY(page.attr.is_cacheable())) {
		entry = {pageno, &page.page()};
	} else if constexpr (memory_traps_enabled) {
		if (UNLIKELY(page.has_trap())) {
			page.trap(address & (Page::size()-1), sizeof(T) | TRAP_WRITE, value);
		}
	}
	page.page().template aligned_write<T>(address & (Page::size()-1), value);
#endif
}

template <int W>
inline address_type<W> Memory<W>::resolve_address(const std::string& name) const
{
	auto* sym = resolve_symbol(name.c_str());
	return (sym) ? sym->st_value : 0x0;
}

template <int W>
inline address_type<W> Memory<W>::resolve_section(const char* name) const
{
	auto* shdr = this->section_by_name(name);
	if (shdr) return shdr->sh_addr;
	return 0x0;
}

template <int W>
inline address_type<W> Memory<W>::exit_address() const noexcept
{
	return this->m_exit_address;
}

template <int W>
inline void Memory<W>::set_exit_address(address_t addr)
{
	this->m_exit_address = addr;
}

template <int W>
inline bool Memory<W>::is_executable(address_t addr, size_t len) const noexcept
{
	return addr >= m_exec_pagedata_base && addr + len < m_exec_pagedata_base + m_exec_pagedata_size;
}
