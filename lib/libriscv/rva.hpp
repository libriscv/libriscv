#pragma once
#include <cstdint>
#include <unordered_set>
#include "types.hpp"

namespace riscv
{
	template <int W>
	struct AtomicMemory
	{
		using address_t = address_type<W>;          // one unsigned memory address

		void load_reserve(address_t addr)
		{
			m_reservations.insert(addr);
		}
		bool store_conditional(address_t addr)
		{
			return m_reservations.erase(addr) != 0;
		}

		std::unordered_set<address_t> m_reservations;
	};
}
