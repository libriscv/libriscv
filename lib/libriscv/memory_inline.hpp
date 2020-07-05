#pragma once

template <int W>
template <typename T>
T Memory<W>::read(address_t address)
{
	const auto pageno = page_number(address);
	if (m_current_rd_page != pageno) {
		const auto* potential = &get_pageno(pageno);
		if (UNLIKELY(!potential->attr.read)) {
			this->protection_fault();
		}
		m_current_rd_page = pageno;
		m_current_rd_ptr = potential;
	}
	const auto& page = *m_current_rd_ptr;

	if constexpr (memory_traps_enabled) {
		if (UNLIKELY(page.has_trap())) {
			return page.trap(address & (Page::size()-1), sizeof(T) | TRAP_READ, 0);
		}
	}
	return page.template aligned_read<T>(address & (Page::size()-1));
}

template <int W>
template <typename T>
void Memory<W>::write(address_t address, T value)
{
	const auto pageno = page_number(address);
	if (m_current_wr_page != pageno) {
		auto* potential = &create_page(pageno);
		if (UNLIKELY(!potential->attr.write)) {
			this->protection_fault();
		}
		m_current_wr_page = pageno;
		m_current_wr_ptr = potential;
	}
	auto& page = *m_current_wr_ptr;

	if constexpr (memory_traps_enabled) {
		if (UNLIKELY(page.has_trap())) {
			page.trap(address & (Page::size()-1), sizeof(T) | TRAP_WRITE, value);
			return;
		}
	}
	page.template aligned_write<T>(address & (Page::size()-1), value);
}

template <int W>
inline const Page& Memory<W>::get_page(const address_t address) const noexcept
{
	const auto page = page_number(address);
	return get_pageno(page);
}

template <int W>
inline Page& Memory<W>::get_exec_pageno(const address_t page)
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

template <int W>
inline Page& Memory<W>::create_page(const address_t pageno)
{
	auto it = m_pages.find(pageno);
	if (it != m_pages.end()) {
		Page& page = it->second;
		if (UNLIKELY(page.attr.is_cow)) {
			page.make_writable();
		}
		return page;
	}
	// create page on-demand, or throw exception when out of memory
	if (this->m_page_fault_handler == nullptr) {
		return default_page_fault(*this, pageno);
	}
	return m_page_fault_handler(*this, pageno);
}

template <int W> inline void
Memory<W>::set_page_attr(address_t dst, size_t len, PageAttributes options)
{
	const bool is_default = options.is_default();
	while (len > 0)
	{
		const size_t size = std::min(Page::size(), len);
		const address_t pageno = page_number(dst);
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
	const address_t pageno = page_number(src);
	const auto& page = this->get_pageno(pageno);
	return page.attr;
}


template <int W> inline void
Memory<W>::invalidate_page(address_t pageno, Page& page)
{
	// it's only possible to a have CoW read-only page
	if (m_current_rd_page == pageno) {
		m_current_rd_ptr = &page;
	}
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
size_t Memory<W>::nonshared_pages_active() const noexcept
{
	return std::accumulate(m_pages.begin(), m_pages.end(),
				0, [] (int value, const auto& it) {
					return value + (!it.second.attr.non_owning ? 1 : 0);
				});
}

template <int W>
void Memory<W>::memset(address_t dst, uint8_t value, size_t len)
{
	while (len > 0)
	{
		const size_t offset = dst & (Page::size()-1); // offset within page
		const size_t size = std::min(Page::size() - offset, len);
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
	while (len != 0)
	{
		const size_t offset = dst & (Page::size()-1); // offset within page
		const size_t size = std::min(Page::size() - offset, len);
		auto& page = this->create_page(dst >> Page::SHIFT);
		std::copy(src, src + size, page.data() + offset);

		dst += size;
		src += size;
		len -= size;
	}
}

template <int W>
void Memory<W>::memcpy_out(void* vdst, address_t src, size_t len) const
{
	auto* dst = (uint8_t*) vdst;
	while (len != 0)
	{
		const size_t offset = src & (Page::size()-1);
		const size_t size = std::min(Page::size() - offset, len);
		const auto& page = this->get_page(src);
		std::copy(page.data() + offset, page.data() + offset + size, dst);

		dst += size;
		src += size;
		len -= size;
	}
}

template <int W>
void Memory<W>::memview(address_t addr, size_t len,
	Function<void(const uint8_t*, size_t)> callback) const
{
	const size_t offset = addr & (Page::size()-1);
	// fast-path
	if (LIKELY(offset + len <= Page::size()))
	{
		const auto& page = this->get_page(addr);
		if (page.has_data()) {
			callback(page.data() + offset, len);
		} else {
			protection_fault();
		}
		return;
	}
	// slow path
	uint8_t* buffer = (uint8_t*) __builtin_alloca(len);
	memcpy_out(buffer, addr, len);
	callback(buffer, len);
}
template <int W>
template <typename T>
void Memory<W>::memview(address_t addr,
	Function<void(const T&)> callback) const
{
	static_assert(std::is_trivial_v<T>, "Type T must be Plain-Old-Data");
	const size_t offset = addr & (Page::size()-1);
	// fast-path
	if (LIKELY(offset + sizeof(T) <= Page::size()))
	{
		const auto& page = this->get_page(addr);
		if (page.has_data()) {
			callback(*(const T*) &page.data()[offset]);
		} else {
			protection_fault();
		}
		return;
	}
	// slow path
	T object;
	memcpy_out(&object, addr, sizeof(object));
	callback(object);
}

template <int W>
std::string Memory<W>::memstring(address_t addr, const size_t max_len) const
{
	std::string result;
	size_t pageno = page_number(addr);
	// fast-path
	{
		address_t offset = addr & (Page::size()-1);
		const Page& page = this->get_pageno(pageno);
		if (UNLIKELY(!page.has_data()))
			protection_fault();

		const char* start = (const char*) &page.data()[offset];
		const char* pgend = (const char*) &page.data()[std::min(Page::size(), offset + max_len)];
		//
		const char* reader = start + strnlen(start, pgend - start);
		// early exit
		if (reader < pgend) {
			return std::string(start, reader);
		}
		// we are crossing a page
		result.append(start, reader);
	}
	// slow-path: cross page-boundary
	while (result.size() < max_len)
	{
		const size_t max_bytes = std::min(Page::size(), max_len - result.size());
		pageno ++;
		const Page& page = this->get_pageno(pageno);
		if (UNLIKELY(!page.has_data()))
			protection_fault();

		const char* start = (const char*) page.data();
		const char* endptr = (const char*) &page.data()[max_bytes];
		//
		const char* reader = start + strnlen(start, max_bytes);
		result.append(start, reader);

		if (reader < endptr) {
			if (*reader == 0) return result;
		}
	}
	return result;
}

template <int W>
int Memory<W>::memcmp(address_t p1, address_t p2, size_t len) const
{
	// NOTE: fast implementation if no pointer crosses page boundary
	const auto pageno1 = this->page_number(p1);
	const auto pageno2 = this->page_number(p2);
	if (pageno1 == ((p1 + len-1) >> Page::SHIFT) &&
		pageno2 == ((p2 + len-1) >> Page::SHIFT)) {
		auto& page1 = this->get_pageno(pageno1);
		auto& page2 = this->get_pageno(pageno2);
		if (UNLIKELY(!page1.has_data() || !page2.has_data()))
			protection_fault();

		const uint8_t* s1 = page1.data() + p1 % Page::SIZE;
		const uint8_t* s2 = page2.data() + p2 % Page::SIZE;
		return __builtin_memcmp(s1, s2, len);
	}
	else // slow path (optimizable)
	{
		uint8_t v1 = 0;
		uint8_t v2 = 0;
		while (len > 0) {
			const auto pageno1 = this->page_number(p1);
			const auto pageno2 = this->page_number(p2);
			auto& page1 = this->get_pageno(pageno1);
			auto& page2 = this->get_pageno(pageno2);
			if (UNLIKELY(!page1.has_data() || !page2.has_data()))
				protection_fault();

			v1 = page1.data()[p1 % Page::SIZE];
			v2 = page2.data()[p2 % Page::SIZE];
			if (v1 != v2) break;
			p1++;
			p2++;
			len--;
		}
		return len == 0 ? 0 : (v1 - v2);
	}
}
template <int W>
int Memory<W>::memcmp(const void* ptr1, address_t p2, size_t len) const
{
	const char* s1 = (const char*) ptr1;
	// NOTE: fast implementation if no pointer crosses page boundary
	const auto pageno2 = this->page_number(p2);
	if (pageno2 == ((p2 + len-1) >> Page::SHIFT)) {
		auto& page2 = this->get_pageno(pageno2);
		if (UNLIKELY(!page2.has_data())) protection_fault();

		const uint8_t* s2 = page2.data() + p2 % Page::SIZE;
		return __builtin_memcmp(s1, s2, len);
	}
	else // slow path (optimizable)
	{
		uint8_t v2 = 0;
		while (len > 0) {
			const auto pageno2 = this->page_number(p2);
			auto& page2 = this->get_pageno(pageno2);
			if (UNLIKELY(!page2.has_data())) protection_fault();

			v2 = page2.data()[p2 % Page::SIZE];
			if (*s1 != v2) break;
			s1++;
			p2++;
			len--;
		}
		return len == 0 ? 0 : (*s1 - v2);
	}
}

template <int W>
inline void Memory<W>::protection_fault()
{
	CPU<W>::trigger_exception(PROTECTION_FAULT);
}

template <int W>
void Memory<W>::trap(address_t page_addr, mmio_cb_t callback)
{
	auto& page = create_page(page_number(page_addr));
	page.set_trap(callback);
}

template <int W>
address_type<W> Memory<W>::resolve_address(const char* name) const
{
	const auto& it = sym_lookup.find(name);
	if (it != sym_lookup.end()) return it->second;

	auto* sym = resolve_symbol(name);
	address_t addr = (sym) ? sym->st_value : 0x0;
	sym_lookup.emplace(strdup(name), addr);
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
