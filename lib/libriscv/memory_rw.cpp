#include "machine.hpp"

namespace riscv
{
	template <int W>
	const Page& Memory<W>::get_readable_pageno(const address_t pageno) const
	{
		const auto& page = get_pageno(pageno);
		if (LIKELY(page.attr.read))
			return page;
		this->protection_fault(pageno * Page::size());
	}

	template <int W>
	Page& Memory<W>::create_writable_pageno(const address_t pageno, bool init)
	{
		auto it = m_pages.find(pageno);
		if (LIKELY(it != m_pages.end())) {
			Page& page = it->second;
			if (LIKELY(page.attr.write)) {
				return page;
			} else if (page.attr.is_cow) {
				m_page_write_handler(*this, pageno, page);
				// The page may be read-cached at this time
				// and the page data has likely changed now.
				this->invalidate_cache(pageno, &page);
				return page;
			}
		} else {
			// Handler must produce a new page, or throw
			Page& page = m_page_fault_handler(*this, pageno, init);
			if (LIKELY(page.attr.write)) {
				this->invalidate_cache(pageno, &page);
				return page;
			}
		}
		this->protection_fault(pageno * Page::size());
	}

	template <int W>
	void Memory<W>::set_pageno_attr(const address_t pageno, PageAttributes attr)
	{
		auto it = pages().find(pageno);
		if (it != pages().end()) {
			auto& page = it->second;
			// Keep non-owning and is_cow attributes
			const bool is_cow = page.attr.is_cow;
			page.attr.apply_regular_attributes(attr);
			if (is_cow || (attr.write && page.is_cow_page())) {
				// If the page becomes writable and holds the CoW-page data, it's also copy-on-write
				page.attr.is_cow = true;
				page.attr.write = false;
			}
			return;
		}

		// Don't create any pages if the defaults apply
		const bool is_default = attr.is_default();
		if (is_default)
			return;

		// Create arena-page
		if (flat_readwrite_arena && pageno < this->m_arena_pages)
		{
			auto& page = this->create_writable_pageno(pageno);
			page.attr.apply_regular_attributes(attr);
			return;
		}
		// Writable: Create a non-owning copy-on-write zero-page
		// Read-only: Create a non-owning zero-page
		attr.is_cow = attr.write;
		attr.write = false;
		attr.non_owning = true;
		m_pages.try_emplace(pageno, attr, Page::cow_page().m_page.get());
	}

	template <int W>
	bool Memory<W>::free_pageno(address_t pageno)
	{
		return m_pages.erase(pageno) != 0;
	}

	template <int W>
	void Memory<W>::free_pages(address_t dst, size_t len)
	{
		address_t pageno = page_number(dst);
		address_t end = pageno + (len /= Page::size());
		while (pageno < end)
		{
			this->free_pageno(pageno);
			pageno ++;
		}
		// TODO: This can be improved by invalidating matches only
		this->invalidate_reset_cache();
	}

	template <int W>
	void Memory<W>::default_page_write(Memory<W>&, address_t, Page& page)
	{
		page.make_writable();
	}

	template <int W>
	const Page& Memory<W>::default_page_read(const Memory<W>& mem, address_t pageno)
	{
		// This is a copy-on-write zeroed area, but we must respect the underlying arena
		if (flat_readwrite_arena && pageno < mem.m_arena_pages) {
			return const_cast<Memory<W>&> (mem).create_writable_pageno(pageno);
		}
		return Page::cow_page();
	}

	static const Page zeroed_page {
		PageAttributes {
			.read   = true,
			.write  = false,
			.exec   = false,
			.is_cow = true
		}
	};
	static const Page guarded_page {
		PageAttributes {
			.read   = false,
			.write  = false,
			.exec   = false,
			.is_cow = false,
			.non_owning = true
		}, nullptr
	};
	static const Page host_codepage {
		PageAttributes {
			.read   = false,
			.write  = false,
			.exec   = true,
			.is_cow = false,
			.non_owning = true
		}, std::array<uint8_t, PageSize> {
			// STOP: 0x7ff00073
			0x73, 0x00, 0xf0, 0x7f,
			// JMP -4 (jump back to STOP): 0xffdff06f
			0x6f, 0xf0, 0xdf, 0xff,
			0x0
		}
	};
	const Page& Page::cow_page() noexcept {
		return zeroed_page; // read-only, zeroed page
	}
	const Page& Page::guard_page() noexcept {
		return guarded_page; // inaccessible page
	}
	const Page& Page::host_page() noexcept {
		return host_codepage; // host code page
	}

	template <int W>
	Page& Memory<W>::install_shared_page(address_t pageno, const Page& shared_page)
	{
		auto& already_there = get_pageno(pageno);
		if (!already_there.is_cow_page() && !already_there.attr.non_owning)
			throw MachineException(ILLEGAL_OPERATION,
				"There was a page at the specified location already", pageno);
		if (shared_page.has_data() == false && (
			shared_page.attr.write || shared_page.attr.read || shared_page.attr.exec))
			throw MachineException(ILLEGAL_OPERATION,
				"There was a RWX page with no allocated data", pageno);

		auto attr = shared_page.attr;
		attr.non_owning = true;
		// NOTE: If you insert a const Page, DON'T modify it! The machine
		// won't, unless system-calls do or manual intervention happens!
		auto res = m_pages.try_emplace(
			pageno,
			attr, const_cast<PageData*> (shared_page.m_page.get())
		);
		// TODO: Can be improved by invalidating more intelligently
		this->invalidate_reset_cache();
		// try overwriting instead, if emplace failed
		if (res.second == false) {
			Page& page = res.first->second;
			new (&page) Page{attr, const_cast<PageData*> (shared_page.m_page.get())};
			return page;
		}
		return res.first->second;
	}

	template <int W>
	void Memory<W>::insert_non_owned_memory(
		address_t dst, void* src, size_t size, PageAttributes attr)
	{
		assert(dst % Page::size() == 0);
		assert((dst + size) % Page::size() == 0);
		attr.non_owning = true;

		for (size_t i = 0; i < size; i += Page::size())
		{
			const auto pageno = (dst + i) / Page::size();
			PageData* pdata = reinterpret_cast<PageData*> ((char*) src + i);
			m_pages.try_emplace(
				pageno,
				attr, pdata
			);
		}
		// TODO: Can be improved by invalidating more intelligently
		this->invalidate_reset_cache();
	}

	template <int W> void
	Memory<W>::set_page_attr(address_t dst, size_t len, PageAttributes attr)
	{
		//printf("set_page_attr(0x%lX, %zu, prot=%X)\n", long(dst), len, attr.to_prot());
		while (len > 0)
		{
			const size_t size = std::min(Page::size(), len);
			const address_t pageno = page_number(dst);
			this->set_pageno_attr(pageno, attr);

			dst += size;
			len -= size;
		}
	}

	template <int W>
	size_t Memory<W>::memory_usage_total() const
	{
		size_t total = 0;
		total += sizeof(Machine<W>);
		// Pages
		for (const auto& it : m_pages) {
			const auto page_number = it.first;
			const auto& page = it.second;
			total += sizeof(page);
				// Regular owned page
			if ((!page.attr.non_owning && page.has_data()) ||
				// Arena page
				(page.attr.non_owning && page_number < m_arena_pages))
					total += Page::size();
		}

		for (const auto& exec : m_exec) {
			total += exec.size_bytes();
		}

		return total;
	}

	template struct Memory<4>;
	template struct Memory<8>;
	INSTANTIATE_128_IF_ENABLED(Memory);
}
