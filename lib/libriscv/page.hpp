#pragma once
#include <cassert>
#include <memory>
#include <stdexcept>
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
	bool shared = false;

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
	using mmio_cb_t = std::function<int64_t (Page&, uint32_t, int, int64_t)>;

	Page() = default;
	Page(const PageAttributes& a, const PageData& d)
		: m_page(d), attr(a) {}

	auto& page() noexcept { return m_page; }
	const auto& page() const noexcept { return m_page; }

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
	auto* decoder_cache() noexcept {
		return m_decoder_cache.get();
	}
	const auto* decoder_cache() const noexcept {
		return m_decoder_cache.get();
	}
	void create_decoder_cache() {
		m_decoder_cache.reset(new DecoderCache<Page::SIZE>);
	}
#endif

	bool has_trap() const noexcept { return m_trap != nullptr; }
	void set_trap(mmio_cb_t newtrap) noexcept { this->m_trap = newtrap; }
	int64_t trap(uint32_t offset, int mode, int64_t value) const;
	static int trap_mode(int mode) noexcept { return mode & 0xF000; }

	int64_t passthrough(uint32_t off, int mode, int64_t val);

	// this combination has been benchmarked to be faster than
	// page-aligning the PageData struct, and avoids an indirection
	PageData m_page;
	PageAttributes attr;
#ifdef RISCV_INSTR_CACHE
	std::unique_ptr<DecoderCache<Page::SIZE>> m_decoder_cache = nullptr;
#endif
	mmio_cb_t m_trap = nullptr;
};

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
