#pragma once
#include <cstdint>
#include <EASTL/fixed_vector.h>
#include "types.hpp"

namespace riscv
{
	template <int W>
	struct AtomicMemory
	{
		using address_t = address_type<W>;          // one unsigned memory address
		static constexpr size_t MAX_RESV = 8;

		void load_reserve(int size, address_t addr)
		{
			check_alignment(size, addr);
			m_reservations.push_back(addr);
		}

		// Volume I: RISC-V Unprivileged ISA V20190608 p.49:
		// An SC can only pair with the most recent LR in program order.
		bool store_conditional(int size, address_t addr)
		{
			check_alignment(size, addr);
			if (UNLIKELY(m_reservations.empty()))
				return false;

			bool result = (addr == m_reservations.back());
			if (result)
				m_reservations.pop_back();
			return result;
		}

	private:
		inline void check_alignment(int size, address_t addr)
		{
			if (UNLIKELY(addr & (size-1))) {
				throw MachineException(INVALID_ALIGNMENT,
					"Load-Reserved address is misaligned", addr);
			}
		}

		eastl::fixed_vector<address_t, MAX_RESV, false> m_reservations;
	};
}
