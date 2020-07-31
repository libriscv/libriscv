#include "machine.hpp"

namespace riscv
{
	template <int W>
	const Page& Memory<W>::get_readable_page(address_t address)
	{
		const auto pageno = page_number(address);
		if (m_current_rd_page != pageno) {
			const auto* potential = &get_pageno(pageno);
			if (UNLIKELY(!potential->attr.read)) {
				this->protection_fault(address);
			}
			m_current_rd_page = pageno;
			m_current_rd_ptr = potential;
		}
		return *m_current_rd_ptr;
	}

	template <int W>
	Page& Memory<W>::get_writable_page(address_t address)
	{
		const auto pageno = page_number(address);
		if (m_current_wr_page != pageno) {
			auto* potential = &create_page(pageno);
			if (UNLIKELY(!potential->attr.write)) {
				this->protection_fault(address);
			}
			m_current_wr_page = pageno;
			m_current_wr_ptr = potential;
		}
		return *m_current_wr_ptr;
	}

	template <int W>
	Page& Memory<W>::create_page(const address_t pageno)
	{
		auto it = m_pages.find(pageno);
		if (it != m_pages.end()) {
			Page& page = it->second;
			if (UNLIKELY(page.attr.is_cow)) {
				m_page_write_handler(*this, page);
			}
			return page;
		}
		// this callback must produce a new page, or throw
		return m_page_fault_handler(*this, pageno);
	}

	template <int W>
	void Memory<W>::free_pages(address_t dst, size_t len)
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
	void Memory<W>::default_page_write(Memory<W>&, Page& page)
	{
		page.make_writable();
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
	const Page& Page::cow_page() noexcept {
		return zeroed_page; // read-only, zeroed page
	}
	const Page& Page::guard_page() noexcept {
		return guarded_page; // inaccessible page
	}

	template <int W>
	Page& Memory<W>::install_shared_page(address_t pageno, const Page& shared_page)
	{
		if (UNLIKELY(get_pageno(pageno).attr.is_cow == false))
			throw MachineException(ILLEGAL_OPERATION,
				"There was a page at the specified location already", pageno);
		if (shared_page.data() == nullptr && (
			shared_page.attr.write || shared_page.attr.read || shared_page.attr.exec))
			throw MachineException(ILLEGAL_OPERATION,
				"There was a RWX page with no allocated data", pageno);

		auto attr = shared_page.attr;
		attr.non_owning = true;
		// NOTE: If you insert a const Page, DON'T modify it! The machine
		// won't, unless system-calls do or manual intervention happens!
		m_pages.try_emplace(pageno, attr, const_cast<PageData*> (&shared_page.page()));
		return m_pages[pageno];
	}

	template <int W>
	void Memory<W>::insert_non_owned_memory(
		address_t dst, void* src, size_t size, PageAttributes attr)
	{
		assert(dst % Page::size() == 0);
		assert(size % Page::size() == 0);
		attr.non_owning = true;

		for (size_t i = 0; i < size; i += Page::size())
		{
			const auto pageno = (dst + i) >> Page::SHIFT;
			PageData* pdata = reinterpret_cast<PageData*> ((char*) src + i);
			m_pages.try_emplace(pageno, attr, pdata);
		}
	}

	template <int W> void
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

	template struct Memory<4>;
	template struct Memory<8>;
}
