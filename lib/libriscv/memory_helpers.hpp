#pragma once

template <int W>
void Memory<W>::memset(address_t dst, uint8_t value, size_t len)
{
	while (len > 0)
	{
		const size_t offset = dst & (Page::size()-1); // offset within page
		const size_t size = std::min(Page::size() - offset, len);
		auto& page = this->create_page(dst >> Page::SHIFT);
		if (UNLIKELY(!page.has_data()))
			protection_fault(dst);

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
		if (UNLIKELY(!page.has_data()))
			protection_fault(dst);

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
		if (UNLIKELY(!page.has_data()))
			protection_fault(src);

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
			protection_fault(addr);
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
			protection_fault(addr);
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
			protection_fault(addr);

		const char* start = (const char*) &page.data()[offset];
		const char* pgend = (const char*) &page.data()[std::min(Page::size(), offset + max_len)];
		//
		const char* reader = start + strnlen(start, pgend - start);
		result.append(start, reader);
		// early exit
		if (LIKELY(reader < pgend)) {
			return result;
		}
	}
	// slow-path: cross page-boundary
	while (result.size() < max_len)
	{
		const size_t max_bytes = std::min(Page::size(), max_len - result.size());
		pageno ++;
		const Page& page = this->get_pageno(pageno);
		if (UNLIKELY(!page.has_data()))
			protection_fault(addr);

		const char* start = (const char*) page.data();
		const char* endptr = (const char*) &page.data()[max_bytes];

		const char* reader = start + strnlen(start, max_bytes);
		result.append(start, reader);
		// if we didn't stop at the page border, we must be done
		if (reader < endptr)
			return result;
	}
	return result;
}

template <int W>
riscv::Buffer Memory<W>::rvbuffer(address_t addr,
	const size_t datalen, const size_t maxlen) const
{
	if (UNLIKELY(datalen + 1 >= maxlen))
		protection_fault(addr);

	riscv::Buffer result;
	size_t pageno = page_number(addr);
	const Page& page = this->get_pageno(pageno);
	if (UNLIKELY(!page.has_data()))
		protection_fault(addr);

	const address_t offset = addr & (Page::size()-1);
	auto* start = (const char*) &page.data()[offset];
	const size_t max_bytes = std::min(Page::size() - offset, datalen);
	result.append_page(start, max_bytes);
	// slow-path: cross page-boundary
	while (result.size() < datalen)
	{
		const size_t max_bytes = std::min(Page::size(), datalen - result.size());
		pageno ++;
		const Page& page = this->get_pageno(pageno);
		if (UNLIKELY(!page.has_data()))
			protection_fault(addr);

		result.append_page((const char*) page.data(), max_bytes);
	}
	return result;
}

template <int W>
size_t Memory<W>::strlen(address_t addr, size_t maxlen) const
{
	size_t len = 0;

	do {
		const address_t offset = addr & (Page::size()-1);
		size_t pageno = page_number(addr);
		const Page& page = this->get_pageno(pageno);
		if (UNLIKELY(!page.has_data()))
			protection_fault(addr);

		const char* start = (const char*) &page.data()[offset];
		const size_t max_bytes = Page::size() - offset;
		const size_t thislen = strnlen(start, max_bytes);
		len += thislen;
		if (thislen != max_bytes) break;
	} while (len < maxlen);

	if (len <= maxlen)
		return len;
	return maxlen;
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
			protection_fault(p1);

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
				protection_fault(p1);

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
		if (UNLIKELY(!page2.has_data())) protection_fault(p2);

		const uint8_t* s2 = page2.data() + p2 % Page::SIZE;
		return __builtin_memcmp(s1, s2, len);
	}
	else // slow path (optimizable)
	{
		uint8_t v2 = 0;
		while (len > 0) {
			const auto pageno2 = this->page_number(p2);
			auto& page2 = this->get_pageno(pageno2);
			if (UNLIKELY(!page2.has_data())) protection_fault(p2);

			v2 = page2.data()[p2 % Page::SIZE];
			if (*s1 != v2) break;
			s1++;
			p2++;
			len--;
		}
		return len == 0 ? 0 : (*s1 - v2);
	}
}
