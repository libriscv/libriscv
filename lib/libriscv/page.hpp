#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <tuple>
template <class...> constexpr std::false_type always_false {};

namespace riscv {

struct PageAttributes
{
	bool read  = true;
	bool write = true;
	bool exec  = false;
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

	auto& page() noexcept { return m_page; }
	const auto& page() const noexcept { return m_page; }

	template <typename T>
	inline T aligned_read(uint32_t offset) const
	{
		if (this->attr.read) {
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
		return T {};
	}

	template <typename T>
	inline void aligned_write(uint32_t offset, T value)
	{
		if (this->attr.write) {
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

	// this combination has been benchmarked to be faster than
	// page-aligning the PageData struct, and avoids an indirection
	PageAttributes attr;
	PageData m_page;
};

}
