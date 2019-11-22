#pragma once
#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <tuple>
#include "common.hpp"
#include "types.hpp"
#include "util/delegate.hpp"
template <class...> constexpr std::false_type always_false {};

namespace riscv {

struct PageAttributes
{
	bool read  = true;
	bool write = true;
	bool exec  = false;
	bool is_cow = false;

	bool is_default() const noexcept {
		PageAttributes def {};
		return this->read == def.read && this->write == def.write && this->exec == def.exec;
	}
};

union PageData {
	static constexpr unsigned SIZE  = 4096;
	static constexpr unsigned SHIFT = 12;

	std::array<uint8_t,  SIZE / 1> buffer8;
	std::array<uint16_t, SIZE / 2> buffer16;
	std::array<uint32_t, SIZE / 4> buffer32;
	std::array<uint64_t, SIZE / 8> buffer64;
};

struct Page
{
	static constexpr unsigned SIZE  = PageData::SIZE;
	static constexpr unsigned SHIFT = PageData::SHIFT;
	using mmio_cb_t = delegate<int64_t (Page&, uint32_t, int, int64_t)>;

	auto& page() noexcept { return m_page; }
	const auto& page() const noexcept { return m_page; }

	template <typename T>
	inline T aligned_read(uint32_t offset) const
	{
		if constexpr (memory_alignment_check) {
			assert(offset % sizeof(T) == 0);
		}
		if constexpr (std::is_same<T, uint8_t>::value) {
			return page().buffer8[offset];
		} else if constexpr (std::is_same<T, uint16_t>::value) {
			return page().buffer16[offset >> 1];
		} else if constexpr (std::is_same<T, uint32_t>::value) {
			return page().buffer32[offset >> 2];
		} else if constexpr (std::is_same<T, uint64_t>::value) {
			return page().buffer64[offset >> 3];
		}
		else {
			static_assert(always_false<T>, "Can't use this type when reading memory");
		}
	}

	template <typename T>
	inline void aligned_write(uint32_t offset, T value)
	{
		if constexpr (memory_alignment_check) {
			assert(offset % sizeof(T) == 0);
		}
		if constexpr (std::is_same<T, uint8_t>::value) {
			page().buffer8[offset] = value;
		} else if constexpr (std::is_same<T, uint16_t>::value) {
			page().buffer16[offset >> 1] = value;
		} else if constexpr (std::is_same<T, uint32_t>::value) {
			page().buffer32[offset >> 2] = value;
		} else if constexpr (std::is_same<T, uint64_t>::value) {
			page().buffer64[offset >> 3] = value;
		}
		else {
			static_assert(always_false<T>, "Can't use this type when writing memory");
		}
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

	bool has_trap() const noexcept { return m_trap != nullptr; }
	void set_trap(mmio_cb_t newtrap) noexcept { this->m_trap = newtrap; }
	int64_t trap(uint32_t offset, int mode, int64_t value) const;
	static int trap_mode(int mode) noexcept { return mode & 0xF000; }

	int64_t passthrough(uint32_t off, int mode, int64_t val);

	// this combination has been benchmarked to be faster than
	// page-aligning the PageData struct, and avoids an indirection
	PageAttributes attr;
	PageData m_page;
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
