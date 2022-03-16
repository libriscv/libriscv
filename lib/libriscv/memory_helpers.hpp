#pragma once

template <int W> inline
void Memory<W>::memzero(address_t dst, size_t len)
{
#ifdef RISCV_RODATA_SEGMENT_IS_SHARED
	// Check if we are trying to memzero the custom RO area
	const address_t p_begin = page_number(dst);
	const address_t p_end = page_number(dst + len);
	if (UNLIKELY(m_ropages.contains(p_begin, p_end))) {
		this->protection_fault(dst);
	}
#endif
	while (len > 0)
	{
		const size_t offset = dst & (Page::size()-1); // offset within page
		const size_t size = std::min(Page::size() - offset, len);
		const address_t pageno = page_number(dst);
		// We only use the page table now because we have previously
		// checked special regions.
		auto it = m_pages.find(pageno);
		// If we don't find a page, we can treat it as a CoW zero page
		if (it != m_pages.end()) {
			Page& page = it->second;
			if (page.attr.is_cow) {
				m_page_write_handler(*this, pageno, page);
			}
			if (page.attr.write) {
				// Zero the existing writable page
				std::memset(page.data() + offset, 0, size);
			} else {
				this->protection_fault(dst);
			}
		} else {
			// Check if the page being read is known to be all zeroes
			const Page& page = m_page_readf_handler(*this, pageno);
			// If not, the page fault gives us a new blank page.
			// Theoretically the handler can do anything, so do the
			// due diligence of checking if the page is writable.
			if (!page.is_cow_page()) {
				auto& new_page = m_page_fault_handler(*this, pageno);
				if (!new_page.attr.write)
					this->protection_fault(dst);
			}
		}

		dst += size;
		len -= size;
	}
}

template <int W> inline
void Memory<W>::memset(address_t dst, uint8_t value, size_t len)
{
	if (value == 0) {
		memzero(dst, len); return;
	}
	while (len > 0)
	{
		const size_t offset = dst & (Page::size()-1); // offset within page
		const size_t size = std::min(Page::size() - offset, len);
		auto& page = this->create_writable_pageno(dst >> Page::SHIFT);

		std::memset(page.data() + offset, value, size);

		dst += size;
		len -= size;
	}
}

template <int W> inline
void Memory<W>::memcpy(address_t dst, const void* vsrc, size_t len)
{
	auto* src = (uint8_t*) vsrc;
	while (len != 0)
	{
		const size_t offset = dst & (Page::size()-1); // offset within page
		const size_t size = std::min(Page::size() - offset, len);
		auto& page = this->create_writable_pageno(dst >> Page::SHIFT);

		std::copy(src, src + size, page.data() + offset);

		dst += size;
		src += size;
		len -= size;
	}
}

template <int W> inline
void Memory<W>::memcpy_out(void* vdst, address_t src, size_t len) const
{
	auto* dst = (uint8_t*) vdst;
	while (len != 0)
	{
		const size_t offset = src & (Page::size()-1);
		const size_t size = std::min(Page::size() - offset, len);
		const auto& page = this->get_page(src);
		if (UNLIKELY(!page.attr.read))
			protection_fault(src);

		std::copy(page.data() + offset, page.data() + offset + size, dst);

		dst += size;
		src += size;
		len -= size;
	}
}

template <int W>
template <typename T>
inline void Memory<W>::foreach_helper(T& mem, address_t addr, size_t len,
	std::function<void(T&, address_t, const uint8_t*, size_t)> callback)
{
	address_t boff = 0;
	while (len != 0)
	{
		const size_t offset = addr & (Page::size()-1);
		const size_t size = std::min(Page::size() - offset, len);
		const auto& page = mem.get_page(addr);
		if (page.attr.read) {
			callback(mem, boff, page.data() + offset, size);
		} else {
			protection_fault(addr);
		}

		addr += size;
		boff += size;
		len  -= size;
	}
}
template <int W>
template <typename T>
inline void Memory<W>::memview_helper(T& mem, address_t addr, size_t len,
	std::function<void(T&, const uint8_t*, size_t)> callback)
{
	const size_t offset = addr & (Page::size()-1);
	// fast-path
	if (LIKELY(offset + len <= Page::size()))
	{
		const auto& page = mem.get_page(addr);
		if (page.attr.read) {
			callback(mem, page.data() + offset, len);
		} else {
			protection_fault(addr);
		}
		return;
	}
	// slow path
	uint8_t* buffer = (uint8_t*) __builtin_alloca(len);
	mem.memcpy_out(buffer, addr, len);
	callback(mem, buffer, len);
}

template <int W>
void Memory<W>::foreach(address_t addr, size_t len,
	std::function<void(const Memory<W>&, address_t, const uint8_t*, size_t)> callback) const
{
	foreach_helper(*this, addr, len, std::move(callback));
}
template <int W>
void Memory<W>::foreach(address_t addr, size_t len,
	std::function<void(Memory<W>&, address_t, const uint8_t*, size_t)> callback)
{
	foreach_helper(*this, addr, len, std::move(callback));
}
template <int W>
void Memory<W>::memview(address_t addr, size_t len,
	std::function<void(const Memory<W>&, const uint8_t*, size_t)> callback) const
{
	memview_helper(*this, addr, len, std::move(callback));
}
template <int W>
void Memory<W>::memview(address_t addr, size_t len,
	std::function<void(Memory<W>&, const uint8_t*, size_t)> callback)
{
	memview_helper(*this, addr, len, std::move(callback));
}
template <int W>
template <typename T>
void Memory<W>::memview(address_t addr,
	std::function<void(const T&)> callback) const
{
	static_assert(std::is_trivial_v<T>, "Type T must be Plain-Old-Data");
	const size_t offset = addr & (Page::size()-1);
	// fast-path
	if (LIKELY(offset + sizeof(T) <= Page::size()))
	{
		const auto& page = this->get_page(addr);
		if (page.attr.read) {
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
	address_t pageno = page_number(addr);
	// fast-path
	{
		const size_t offset = addr & (Page::size()-1);
		const Page& page = this->get_readable_pageno(pageno);

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
		const Page& page = this->get_readable_pageno(pageno);

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

	address_t pageno = page_number(addr);
	const Page& page = this->get_readable_pageno(pageno);

	const size_t offset = addr & (Page::size()-1);
	auto* start = (const char*) &page.data()[offset];
	const size_t max_bytes = std::min(Page::size() - offset, datalen);

	riscv::Buffer result;
	result.append_page(start, max_bytes);
	// slow-path: cross page-boundary
	while (result.size() < datalen)
	{
		const size_t max_bytes = std::min(Page::size(), datalen - result.size());
		pageno ++;
		const Page& page = this->get_readable_pageno(pageno);

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
		const address_t pageno = page_number(addr);
		const Page& page = this->get_readable_pageno(pageno);

		const char* start = (const char*) &page.data()[offset];
		const size_t max_bytes = Page::size() - offset;
		const size_t thislen = strnlen(start, max_bytes);
		len += thislen;
		if (thislen != max_bytes) break;
		addr += len;
	} while (len < maxlen);

	return (len <= maxlen) ? len : maxlen;
}

template <int W>
int Memory<W>::memcmp(address_t p1, address_t p2, size_t len) const
{
	// NOTE: fast implementation if no pointer crosses page boundary
	const auto pageno1 = this->page_number(p1);
	const auto pageno2 = this->page_number(p2);
	if (pageno1 == ((p1 + len-1) >> Page::SHIFT) &&
		pageno2 == ((p2 + len-1) >> Page::SHIFT)) {
		auto& page1 = this->get_readable_pageno(pageno1);
		auto& page2 = this->get_readable_pageno(pageno2);

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
			auto& page1 = this->get_readable_pageno(pageno1);
			auto& page2 = this->get_readable_pageno(pageno2);

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
		auto& page2 = this->get_readable_pageno(pageno2);

		const uint8_t* s2 = page2.data() + p2 % Page::SIZE;
		return __builtin_memcmp(s1, s2, len);
	}
	else // slow path (optimizable)
	{
		uint8_t v2 = 0;
		while (len > 0) {
			const auto pageno2 = this->page_number(p2);
			auto& page2 = this->get_readable_pageno(pageno2);

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
void Memory<W>::memcpy(
	address_t dst, Machine<W>& srcm, address_t src, address_t len)
{
	if ((dst & (W-1)) == (src & (W-1))) {
		while ((src & (W-1)) != 0 && len > 0) {
			this->template write<uint8_t> (dst++,
				srcm.memory.template read<uint8_t> (src++));
			len --;
		}
		while (len >= 16) {
			this->template write<uint32_t> (dst + 0,
				srcm.memory.template read<uint32_t> (src + 0));
			this->template write<uint32_t> (dst + 1*W,
				srcm.memory.template read<uint32_t> (src + 1*W));
			this->template write<uint32_t> (dst + 2*W,
				srcm.memory.template read<uint32_t> (src + 2*W));
			this->template write<uint32_t> (dst + 3*W,
				srcm.memory.template read<uint32_t> (src + 3*W));
			dst += 16; src += 16; len -= 16;
		}
		while (len >= W) {
			this->template write<uint32_t> (dst,
				srcm.memory.template read<uint32_t> (src));
			dst += W; src += W; len -= W;
		}
	}
	while (len > 0) {
		this->template write<uint8_t> (dst++,
			srcm.memory.template read<uint8_t> (src++));
		len --;
	}
}

template <int W>
size_t Memory<W>::gather_buffers_from_range(
	size_t cnt, vBuffer buffers[], address_t addr, size_t len)
{
	size_t index = 0;
	vBuffer* last = nullptr;
	while (len != 0 && index < cnt)
	{
		const size_t offset = addr & (Page::SIZE-1);
		const size_t size = std::min(Page::SIZE - offset, len);
		auto& page = get_readable_pageno(page_number(addr));

		auto* ptr = (char*) &page.data()[offset];
		if (last && ptr == last->ptr + last->len) {
			last->len += size;
		} else {
			last = &buffers[index];
			last->ptr = ptr;
			last->len = size;
			index ++;
		}
		addr += size;
		len -= size;
	}
	if (UNLIKELY(len != 0)) {
		throw MachineException(OUT_OF_MEMORY, "Out of buffers", index);
	}
	return index;
}
