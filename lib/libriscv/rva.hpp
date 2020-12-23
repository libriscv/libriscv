#pragma once
#include <cstdint>
#include <stdexcept>
#include <EASTL/hash_set.h>
#include "types.hpp"

namespace riscv
{
	template <int W>
	struct AtomicMemory
	{
		using address_t = address_type<W>;          // one unsigned memory address
		static constexpr size_t MAX_RESV = 16;

		void load_reserve(int size, address_t addr)
		{
			check_alignment(size, addr);
			if (LIKELY(m_reservations.size() < MAX_RESV))
				m_reservations.insert(addr);
#ifdef __exceptions
			else
				throw MachineException(DEADLOCK_REACHED,
					"Not enough room for memory reservations", addr);
#endif
		}

		// Volume I: RISC-V Unprivileged ISA V20190608 p.49:
		// An SC can only pair with the most recent LR in program order.
		bool store_conditional(int size, address_t addr)
		{
			check_alignment(size, addr);
			return (m_reservations.erase(addr) != 0);
		}

	private:
		inline void check_alignment(int size, address_t addr)
		{
			if (UNLIKELY(addr & (size-1))) {
#ifdef __exceptions
				throw MachineException(INVALID_ALIGNMENT,
					"Load-Reserved address is misaligned", addr);
#endif
			}
		}

		eastl::hash_set<address_t> m_reservations;
	};
}
