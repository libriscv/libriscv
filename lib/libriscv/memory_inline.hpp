#pragma once
// Force-align memory operations to their native alignments
template <typename T> constexpr inline size_t memory_align_mask() {
	return size_t(Page::size() - 1); // & ~size_t(sizeof(T)-1);
}

template <int W>
template <typename T> inline
T Memory<W>::read(address_t address)
{
	const auto& pagedata = cached_readable_page(address, sizeof(T));
	return pagedata.template aligned_read<T>(address & memory_align_mask<T>());
}

template <int W>
template <typename T> inline
T& Memory<W>::writable_read(address_t address)
{
	auto& pagedata = cached_writable_page(address);
	return pagedata.template aligned_read<T>(address & memory_align_mask<T>());
}

template <int W>
template <typename T> inline
void Memory<W>::write(address_t address, T value)
{
	const auto pageno = page_number(address);
	auto& entry = m_wr_cache;
	if (entry.pageno == pageno) {
		entry.page->template aligned_write<T>(address & memory_align_mask<T>(), value);
		return;
	}

	auto& page = create_writable_pageno(pageno);
	if (LIKELY(page.attr.is_cacheable())) {
		entry = {pageno, &page.page()};
	} else if constexpr (memory_traps_enabled) {
		if (UNLIKELY(page.has_trap())) {
			page.trap(address & memory_align_mask<T>(), sizeof(T) | TRAP_WRITE, value);
		}
	}
	page.page().template aligned_write<T>(address & memory_align_mask<T>(), value);
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
inline address_type<W> Memory<W>::mmap_allocate(address_t bytes)
{
	// Bytes rounded up to nearest PageSize.
	const address_t result = this->m_mmap_address;
	this->m_mmap_address += (bytes + PageMask) & ~address_t{PageMask};
	return result;
}

template <int W>
inline bool Memory<W>::mmap_relax(address_t addr, address_t size, address_t new_size)
{
	// Undo or relax the last mmap allocation. Returns true if successful.
	if (this->m_mmap_address == addr + size && new_size <= size) {
		this->m_mmap_address = (addr + new_size + PageMask) & ~address_t{PageMask};
		return true;
	}
	return false;
}

template <int W>
inline bool Memory<W>::is_executable(address_t addr, size_t len) const noexcept
{
	return addr >= m_exec_pagedata_base && addr + len < m_exec_pagedata_base + m_exec_pagedata_size;
}
