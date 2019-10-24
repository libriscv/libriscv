#pragma once

template <int W>
template <int SIZE>
auto Memory<W>::read(address_t address) {
	auto& page = get_page(address);
	auto value = page.page().template aligned_value<SIZE>(address & (Page::size()-1));
	if (page.attr.read && check_trap(address, SIZE, value)) {
		return value;
	}
	else {
		this->protection_fault();
	}
	return (decltype(value)) 0;
}

template <int W>
template <int SIZE, typename T>
void Memory<W>::write(address_t address, T value) {
	if (check_trap(address, SIZE | 0x1000, value)) {
		auto& page = get_page(address);
		if (page.attr.write) {
			page.page().template aligned_value<SIZE>(address & (Page::size()-1)) = value;
		}
		else {
			this->protection_fault();
		}
	}
}

template <int W>
inline Page& Memory<W>::get_page(const address_t address)
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
		auto& page = this->get_page(dst);
		page.attr = options;

		dst += size;
		len -= size;
	}
}

template <int W>
auto Memory<W>::memset(address_t dst, uint8_t value, size_t len)
{
	while (len > 0)
	{
		const size_t offset = dst & (Page::size()-1); // offset within page
		const size_t remaining = (offset == 0) ? Page::size() : (Page::size() - offset);
		const size_t size = std::min(remaining, len);
		auto& page = this->get_page(dst);
		__builtin_memset(page.data() + offset, value, size);

		dst += size;
		len -= size;
	}
	return dst;
}

template <int W>
auto Memory<W>::memcpy(address_t dst, const uint8_t* src, size_t len)
{
	while (len > 0)
	{
		const size_t offset = dst & (Page::size()-1); // offset within page
		const size_t remaining = (offset == 0) ? Page::size() : (Page::size() - offset);
		const size_t size = std::min(remaining, len);
		auto& page = this->get_page(dst);
		std::memcpy(page.data() + offset, src, size);

		dst += size;
		src += size;
		len -= size;
	}
	return dst;
}

template <int W>
auto* Memory<W>::memcpy_out(uint8_t* dst, address_t src, size_t len)
{
	while (len > 0)
	{
		const size_t offset = src & (Page::size()-1);
		const size_t remaining = (offset == 0) ? Page::size() : (Page::size() - offset);
		const size_t size = std::min(remaining, len);
		auto& page = this->get_page(src);
		std::memcpy(dst, page.data() + offset, size);

		dst += size;
		src += size;
		len -= size;
	}
	return dst;
}
