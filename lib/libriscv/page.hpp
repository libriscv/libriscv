#pragma once
#include "common.hpp"
#include "types.hpp"
#include <cassert>
#include <memory>

namespace riscv {

struct PageAttributes
{
	bool read = true;
	bool write = true;
	bool exec = false;
	bool is_cow = false;
	bool non_owning = false;
	bool unused = false;
	uint16_t user_defined = 0; /* Use this for yourself */

	bool is_default() const noexcept {
		PageAttributes def {};
		return this->read == def.read && this->write == def.write && this->exec == def.exec;
	}
};

struct alignas(8) PageData {
	std::array<uint8_t, PageSize> buffer8 = {0};
};

struct Page
{
	static constexpr unsigned SIZE  = PageSize;
	static constexpr unsigned SHIFT = 31 - __builtin_clz(PageSize);
	static_assert((1u << SHIFT) == PageSize, "Page shift value must match page size");

	using mmio_cb_t = std::function<int64_t (Page&, uint32_t, int, int64_t)>;

	// create a new blank page
	Page() { m_page.reset(new PageData {}); };
	// copy another page (or data)
	Page(const PageAttributes& a, const PageData& d = {})
		: attr(a), m_page(new PageData{d}) { attr.non_owning = false; }
	Page(Page&& other)
		: attr(other.attr), m_page(std::move(other.m_page)) {}
	Page& operator= (Page&& other) {
		attr = other.attr;
		m_page = std::move(other.m_page);
		return *this;
	}
	// create a page that doesn't own this memory
	Page(const PageAttributes& a, PageData* data);
	// don't try to free non-owned page memory
	~Page() {
		if (attr.non_owning) m_page.release();
	}

	auto& page() noexcept { return *m_page; }
	const auto& page() const noexcept { return *m_page; }

	std::string to_string() const;

	template <typename T>
	inline T& aligned_read(uint32_t offset) const
	{
		if constexpr (memory_alignment_check) {
			if (offset % sizeof(T))
				throw MachineException(INVALID_ALIGNMENT, "Misaligned read", offset);
		}
		return *(T*) &page().buffer8[offset];
	}

	template <typename T>
	inline void aligned_write(uint32_t offset, T value)
	{
		if constexpr (memory_alignment_check) {
			if (offset % sizeof(T))
				throw MachineException(INVALID_ALIGNMENT, "Misaligned write", offset);
		}
		*(T*) &page().buffer8[offset] = value;
	}

	auto* data() noexcept {
		return page().buffer8.data();
	}
	const auto* data() const noexcept {
		return page().buffer8.data();
	}

	static constexpr size_t size() noexcept {
		return SIZE;
	}

	bool is_cow_page() const noexcept { return this == &cow_page(); }

	static const Page& cow_page() noexcept;
	static const Page& guard_page() noexcept;

	/* Transform a CoW-page to an owned writable page */
	void make_writable()
	{
		if (m_page != nullptr)
		{
			auto* new_data = new PageData {*m_page};
			if (attr.non_owning) m_page.release();
			m_page.reset(new_data);
		} else {
			m_page.reset(new PageData {});
		}
		attr.write = true;
		attr.is_cow = false;
		attr.non_owning = false;
	}

	// this combination has been benchmarked to be faster than
	// page-aligning the PageData struct and putting it first
	PageAttributes attr;
	std::unique_ptr<PageData> m_page;
#ifdef RISCV_PAGE_TRAPS_ENABLED
	bool has_trap() const noexcept { return m_trap != nullptr; }
	void set_trap(mmio_cb_t newtrap) noexcept { this->m_trap = newtrap; }
	int64_t trap(uint32_t offset, int mode, int64_t value) const;
	static int trap_mode(int mode) noexcept { return mode & 0xF000; }

	int64_t passthrough(uint32_t off, int mode, int64_t val);

	mmio_cb_t m_trap = nullptr;
#endif
};

inline Page::Page(const PageAttributes& a, PageData* data)
	: attr(a)
{
	attr.non_owning = true;
	m_page.reset(data);
}

inline std::string Page::to_string() const
{
	return "Readable: " + std::string(attr.read ? "[x]" : "[ ]") +
		"  Writable: " + std::string(attr.write ? "[x]" : "[ ]") +
		"  Executable: " + std::string(attr.exec ? "[x]" : "[ ]");
}

#ifdef RISCV_PAGE_TRAPS_ENABLED
inline int64_t Page::trap(uint32_t offset, int mode, int64_t value) const
{
	return m_trap((Page&) *this, offset, mode, value);
}

inline int64_t Page::passthrough(uint32_t off, int mode, int64_t val)
{
	switch (mode) {
		case TRAP_READ | 1:
			return this->template aligned_read<uint8_t> (off);
		case TRAP_READ | 2:
			return this->template aligned_read<uint16_t> (off);
		case TRAP_READ | 4:
			return this->template aligned_read<uint32_t> (off);
		case TRAP_READ | 8:
			return this->template aligned_read<uint64_t> (off);
		case TRAP_WRITE | 1:
			this->template aligned_write<uint8_t> (off, val);
			return 0;
		case TRAP_WRITE | 2:
			this->template aligned_write<uint16_t> (off, val);
			return 0;
		case TRAP_WRITE | 4:
			this->template aligned_write<uint32_t> (off, val);
			return 0;
		case TRAP_WRITE | 8:
			this->template aligned_write<uint64_t> (off, val);
			return 0;
	}
	throw MachineException(INVALID_ALIGNMENT, "passthrough: Unknown mode or size", mode);
}
#endif

// Helper class for caching pages
template <int W, typename T> struct CachedPage {
	address_type<W> pageno = (address_type<W>)-1;
	T* page = nullptr;

	void reset() { pageno = (address_type<W>)-1; page = nullptr; }
};

}
