#pragma once

template <int W>
template <typename T>
T Memory<W>::read(address_t address)
{
	const auto pageno = page_number(address);
	if (m_current_rd_page != pageno) {
		m_current_rd_page = pageno;
		m_current_rd_ptr = &get_pageno(pageno);
	}
	const auto& page = *m_current_rd_ptr;

	if constexpr (memory_debug_enabled) {
		if (UNLIKELY(page.has_trap())) {
			return page.trap(address & (Page::size()-1), sizeof(T) | TRAP_READ, 0);
		}
	}
	if (LIKELY(page.attr.read)) {
		return page.template aligned_read<T>(address & (Page::size()-1));
	}
	this->protection_fault();
	return T {};
}

template <int W>
template <typename T>
void Memory<W>::write(address_t address, T value)
{
	const auto pageno = page_number(address);
	if (m_current_wr_page != pageno) {
		m_current_wr_page = pageno;
		m_current_wr_ptr = &create_page(pageno);
		// it's possible for writes to invalidate the cached read page
		if (m_current_rd_page == m_current_wr_page) {
			m_current_rd_ptr = m_current_wr_ptr;
		}
	}
	auto& page = *m_current_wr_ptr;

	if constexpr (memory_debug_enabled) {
		if (UNLIKELY(page.has_trap())) {
			page.trap(address & (Page::size()-1), sizeof(T) | TRAP_WRITE, value);
			return;
		}
	}
	if (LIKELY(page.attr.write)) {
		page.template aligned_write<T>(address & (Page::size()-1), value);
		return;
	}
	this->protection_fault();
}

template <int W>
inline const Page& Memory<W>::get_page(const address_t address) const noexcept
{
	const auto page = page_number(address);
	return get_pageno(page);
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

template <int W>
inline Page& Memory<W>::create_page(const address_t page)
{
	auto it = m_pages.find(page);
	if (it != m_pages.end()) {
		return it->second;
	}
	// create page on-demand, or throw exception when out of memory
	if (this->m_page_fault_handler == nullptr) {
		return default_page_fault(*this, page);
	}
	return m_page_fault_handler(*this, page);
}

template <int W> inline void
Memory<W>::set_page_attr(address_t dst, size_t len, PageAttributes options)
{
	const bool is_default = options.is_default();
	while (len > 0)
	{
		const size_t size = std::min(Page::size(), len);
		const size_t pageno = dst >> Page::SHIFT;
		// unfortunately, have to create pages for non-default attrs
		if (!is_default) {
			this->create_page(pageno).attr = options;
		} else {
			// set attr on non-COW pages only!
			const auto& page = this->get_pageno(pageno);
			if (page.attr.is_cow == false) {
				// this page has been written to, or had attrs set,
				// otherwise it would still be CoW.
				this->create_page(pageno).attr = options;
			}
		}

		dst += size;
		len -= size;
	}
}
template <int W> inline
const PageAttributes& Memory<W>::get_page_attr(address_t src) const noexcept
{
	const size_t pageno = src >> Page::SHIFT;
	const auto& page = this->get_pageno(pageno);
	return page.attr;
}

template <int W> inline void
Memory<W>::free_pages(address_t dst, size_t len)
{
	while (len > 0)
	{
		const size_t size = std::min(Page::size(), len);
		const address_t pageno = dst >> Page::SHIFT;
		auto& page = this->get_pageno(pageno);
		if (page.attr.is_cow == false) {
			m_pages.erase(pageno);
		}
		dst += size;
		len -= size;
	}
}

template <int W>
void Memory<W>::memset(address_t dst, uint8_t value, size_t len)
{
	while (len > 0)
	{
		const size_t offset = dst & (Page::size()-1); // offset within page
		const size_t remaining = (offset == 0) ? Page::size() : (Page::size() - offset);
		const size_t size = std::min(remaining, len);
		auto& page = this->create_page(dst >> Page::SHIFT);
		__builtin_memset(page.data() + offset, value, size);

		dst += size;
		len -= size;
	}
}

template <int W>
void Memory<W>::memcpy(address_t dst, const void* vsrc, size_t len)
{
	auto* src = (uint8_t*) vsrc;
	while (len > 0)
	{
		const size_t offset = dst & (Page::size()-1); // offset within page
		const size_t remaining = (offset == 0) ? Page::size() : (Page::size() - offset);
		const size_t size = std::min(remaining, len);
		auto& page = this->create_page(dst >> Page::SHIFT);
		std::memcpy(page.data() + offset, src, size);

		dst += size;
		src += size;
		len -= size;
	}
}

template <int W>
void Memory<W>::memcpy_out(void* vdst, address_t src, size_t len)
{
	auto* dst = (uint8_t*) vdst;
	while (len > 0)
	{
		const size_t offset = src & (Page::size()-1);
		const size_t remaining = (offset == 0) ? Page::size() : (Page::size() - offset);
		const size_t size = std::min(remaining, len);
		const auto& page = this->get_page(src);
		std::memcpy(dst, page.data() + offset, size);

		dst += size;
		src += size;
		len -= size;
	}
}

template <int W>
inline void Memory<W>::protection_fault()
{
	machine().cpu.trigger_exception(PROTECTION_FAULT);
}

template <int W>
void Memory<W>::trap(address_t page_addr, mmio_cb_t callback)
{
	auto& page = create_page(page_number(page_addr));
	page.set_trap(callback);
}
