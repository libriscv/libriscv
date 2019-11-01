#pragma once

template <int W>
template <typename T>
T Memory<W>::read(address_t address)
{
	if constexpr (memory_debug_enabled) {
		if (!check_trap(address, sizeof(T) | TRAP_READ, 0)) return T{};
	}
	const auto& page = get_page(address);
	if (page.attr.read) {
		return page.template aligned_read<T>(address & (Page::size()-1));
	}
	this->protection_fault();
	return T {};
}

template <int W>
template <typename T>
bool Memory<W>::write(address_t address, T value)
{
	if constexpr (memory_debug_enabled) {
		if (!check_trap(address, sizeof(T) | TRAP_WRITE, value)) return T{};
	}
	auto& page = create_page(address);
	if (page.attr.write) {
		page.template aligned_write<T>(address & (Page::size()-1), value);
		return true;
	}
	this->protection_fault();
	return false;
}

template <int W>
inline const Page& Memory<W>::get_page(const address_t address) const noexcept
{
	const auto page = page_number(address);
	// find existing memory pages
	auto it = m_pages.find(page);
	if (it != m_pages.end()) {
		return it->second;
	}
	return Page::cow_page();
}

template <int W>
inline Page& Memory<W>::create_page(const address_t address)
{
	const auto page = page_number(address);
	// find existing memory pages
	auto it = m_pages.find(page);
	if (it != m_pages.end()) {
		return it->second;
	}
	if (this->m_page_fault_handler == nullptr) {
		return default_page_fault(*this, page);
	}
	return m_page_fault_handler(*this, page);
}

template <int W>
void Memory<W>::set_page_attr(address_t dst, size_t len, PageAttributes options)
{
	while (len > 0)
	{
		const size_t size = std::min(Page::size(), len);
		auto& page = this->create_page(dst);
		page.attr = options;

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
		auto& page = this->create_page(dst);
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
		auto& page = this->create_page(dst);
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

#ifdef RISCV_DEBUG

template <int W>
void Memory<W>::trap(address_t address, mmio_cb_t callback)
{
	if (callback) {
		this->m_callbacks[address] = callback;
	}
	else {
		this->m_callbacks.erase(address);
	}
}
template <int W> constexpr bool
Memory<W>::check_trap(address_t address, int size, address_t value)
{
	if (this->m_callbacks.empty()) return true;
	auto it = m_callbacks.find(address);
	if (it == m_callbacks.end()) return true;
	// do the thing
	return it->second(*this, address, size, value);
}

#else

template <int W> constexpr bool
Memory<W>::check_trap(address_t, int, address_t) {
	return true;
}

#endif
