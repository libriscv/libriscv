#pragma once
#include <array>
#include <cstdint>

struct PageAttributes
{
	bool read  = true;
	bool write = true;
	bool exec  = false;
};

struct alignas(4096) Page {
	static constexpr unsigned SIZE  = 4096;
	static constexpr unsigned SHIFT = 12;

	uint8_t* data() noexcept {
		return memory.buffer8.data();
	}
	static constexpr size_t size() noexcept {
		return SIZE;
	}

	template <int SIZE>
	inline auto& aligned_value(uint32_t offset)
	{
		if constexpr (SIZE == 8) {
			return memory.buffer8.at(offset);
		} else if constexpr (SIZE == 16) {
			return memory.buffer16.at((offset >> 1) & 2047);
		} else if constexpr (SIZE == 32) {
			return memory.buffer32.at((offset >> 2) & 1023);
		} else if constexpr (SIZE == 64) {
			return memory.buffer64.at((offset >> 3) & 511);
		}
		throw std::runtime_error("Invalid read");
	}

	union {
		std::array<uint8_t,  SIZE / 1> buffer8;
		std::array<uint16_t, SIZE / 2> buffer16;
		std::array<uint32_t, SIZE / 4> buffer32;
		std::array<uint64_t, SIZE / 8> buffer64;
	} memory;
};
