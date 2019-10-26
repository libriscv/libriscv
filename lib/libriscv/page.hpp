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

union alignas(4096) PageData {
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

	template <typename T>
	inline std::tuple<T, bool> aligned_read(uint32_t offset) const
	{
		if (this->attr.read) {
			if constexpr (std::is_same<T, uint8_t>::value) {
				return { m_page->buffer8.at(offset), true };
			} else if constexpr (std::is_same<T, uint16_t>::value) {
				return { m_page->buffer16.at((offset >> 1) & 2047), (offset & 0x1) == 0 };
			} else if constexpr (std::is_same<T, uint32_t>::value) {
				return { m_page->buffer32.at((offset >> 2) & 1023), (offset & 0x3) == 0 };
			} else if constexpr (std::is_same<T, uint64_t>::value) {
				return { m_page->buffer64.at((offset >> 3) & 511), (offset & 0x7) == 0 };
			}
			else {
				static_assert(always_false<T>, "Can't use this type when reading memory");
			}
		}
		return { T {}, false };
	}

	template <typename T>
	inline bool aligned_write(uint32_t offset, T value)
	{
		if (this->attr.write) {
			if constexpr (std::is_same<T, uint8_t>::value) {
				m_page->buffer8.at(offset) = value;
				return true;
			} else if constexpr (std::is_same<T, uint16_t>::value) {
				m_page->buffer16.at((offset >> 1) & 2047) = value;
				return (offset & 0x1) == 0;
			} else if constexpr (std::is_same<T, uint32_t>::value) {
				m_page->buffer32.at((offset >> 2) & 1023) = value;
				return (offset & 0x3) == 0;
			} else if constexpr (std::is_same<T, uint64_t>::value) {
				m_page->buffer64.at((offset >> 3) & 511) = value;
				return (offset & 0x7) == 0;
			}
			else {
				static_assert(always_false<T>, "Can't use this type when writing memory");
			}
		}
		return false;
	}

	auto& page() noexcept { return *m_page; }
	const auto& page() const noexcept { return *m_page; }

	auto* data() noexcept {
		return page().buffer8.data();
	}
	const auto* data() const noexcept {
		return page().buffer8.data();
	}

	static constexpr size_t size() noexcept {
		return SIZE;
	}

	static const Page& zero_page() noexcept;

	std::unique_ptr<PageData> m_page { new PageData };
	PageAttributes attr;
};

}
