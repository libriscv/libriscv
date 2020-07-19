#pragma once
#include <cassert>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include "common.hpp"
#include "decoder_cache.hpp"
#include "util/function.hpp"

namespace riscv {

struct PageAttributes
{
	bool read = true;
	bool write = true;
	bool exec = false;
	bool is_cow = false;
	bool non_owning = false;
	bool dont_fork = false;
	uint16_t user_defined = 0; /* Use this for yourself */

	bool is_default() const noexcept {
		PageAttributes def {};
		return this->read == def.read && this->write == def.write && this->exec == def.exec;
	}
};

struct alignas(8) PageData {
	static constexpr unsigned SIZE  = 4096;
	static constexpr unsigned SHIFT = 12;

	std::array<uint8_t, SIZE> buffer8 = {0};
};

struct Page
{
	static constexpr unsigned SIZE  = PageData::SIZE;
	static constexpr unsigned SHIFT = PageData::SHIFT;
	using mmio_cb_t = Function<int64_t (Page&, uint32_t, int, int64_t)>;

	// create a new blank page
	Page() { m_page.reset(new PageData {}); };
	// copy another page (or data)
	Page(const PageAttributes& a, const PageData& d = {})
		: attr(a), m_page(new PageData{d}) {}
	// create a page that doesn't own this memory
	Page(const PageAttributes& a, PageData* data);
	// don't try to free non-owned page memory
	~Page() {
		if (attr.non_owning) m_page.release();
#ifdef RISCV_INSTR_CACHE
		if (m_decoder_non_owned) m_decoder_cache.release();
#endif
	}

	auto& page() noexcept { return *m_page; }
	const auto& page() const noexcept { return *m_page; }

	std::string to_string() const;

	template <typename T>
	inline T aligned_read(uint32_t offset) const
	{
		if constexpr (memory_alignment_check) {
			assert(offset % sizeof(T) == 0);
		}
		return *(T*) &page().buffer8[offset];
	}

	template <typename T>
	inline void aligned_write(uint32_t offset, T value)
	{
		if constexpr (memory_alignment_check) {
			assert(offset % sizeof(T) == 0);
		}
		*(T*) &page().buffer8[offset] = value;
	}

	bool has_data() const noexcept {
		return m_page != nullptr;
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

	static const Page& cow_page() noexcept;
	static const Page& guard_page() noexcept;

#ifdef RISCV_INSTR_CACHE
	auto* decoder_cache() const noexcept {
		return m_decoder_cache.get();
	}
	void create_decoder_cache() const {
		m_decoder_cache.reset(new DecoderCache<Page::SIZE>);
	}
#endif

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
		attr.is_cow = false;
		attr.non_owning = false;
	}

	bool has_trap() const noexcept { return m_trap != nullptr; }
	void set_trap(mmio_cb_t newtrap) noexcept { this->m_trap = newtrap; }
	int64_t trap(uint32_t offset, int mode, int64_t value) const;
	static int trap_mode(int mode) noexcept { return mode & 0xF000; }

	int64_t passthrough(uint32_t off, int mode, int64_t val);

	// this combination has been benchmarked to be faster than
	// page-aligning the PageData struct and putting it first
	PageAttributes attr;
	std::unique_ptr<PageData> m_page;
#ifdef RISCV_INSTR_CACHE
	mutable std::unique_ptr<DecoderCache<Page::SIZE>> m_decoder_cache = nullptr;
	bool m_decoder_non_owned = false;
#endif
	mmio_cb_t m_trap = nullptr;
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
	throw std::runtime_error("passthrough: Unknown mode or size");
}

}
