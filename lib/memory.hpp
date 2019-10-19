#pragma once
#include "types.hpp"
#include "util/delegate.hpp"
#include <stdexcept>
#include <unordered_map>

namespace riscv
{
	template<int W> struct Machine;

	template<int W>
	struct Memory
	{
		using address_t = address_type<W>;
		using isa_t     = isa_type<W>;
		using mmio_cb_t = delegate<void(Memory&, address_t)>;

		Memory(Machine<W>&);

		template <int SIZE>
		inline auto read(address_t address) {
			assert(address & 0x3);
			auto& page = get_page(address);
			switch (SIZE) {
				case 8:
					return page.buffer8.at((address >> 0) & 4095);
				case 16:
					return page.buffer16.at((address >> 1) & 2047);
				case 32:
					return page.buffer32.at((address >> 2) & 1023);
				case 64:
					return page.buffer64.at((address >> 3) & 511);
			}
			throw std::runtime_error("Invalid read");
		}

	private:
		union alignas(4096) page {
			std::array<uint8_t,  4096> buffer8;
			std::array<uint16_t, 2048> buffer16;
			std::array<uint32_t, 1024> buffer32;
			std::array<uint64_t, 512>  buffer64;
		};
		inline page& get_page(const address_t address) {
			const uint32_t page = address >> 12u;
			// creates pages on-demand
			return m_pages[page];
		}

		Machine<W>& m_machine;
		// map of page-indexed trap functions
		// NOTE: uses page-numbers, not byte-addressing
		std::unordered_map<address_t, mmio_cb_t> m_callbacks;

		std::unordered_map<address_t, page> m_pages;
	};

	template <int W>
	inline Memory<W>::Memory(Machine<W>& machine)
		: m_machine(machine)
	{

	}
}
