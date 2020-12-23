#pragma once
#include <cstdint>
#ifdef RISCV_TRANSLATION_DYLIB
#include "common.hpp"
#include "registers.hpp"
#endif

namespace riscv {
#ifdef RISCV_TRANSLATION_DYLIB
	// Thin variant of CPU for higher compilation speed
	template <int W> class CPU {
	public:
		using address_t = address_type<W>;          // one unsigned memory address
		using format_t  = instruction_format<W>; // one machine instruction

		void increment_pc(int delta) { m_regs.pc += delta; }
		auto& reg(uint32_t idx) { return m_regs.get(idx); }
		const auto& reg(uint32_t idx) const { return m_regs.get(idx); }

		Registers<W> m_regs;
		Machine<W>&  m_machine;
	};
#endif

	template <int W>
	struct CallbackTable {
		uint8_t  (*mem_read8)(CPU<W>&, address_type<W> addr);
		uint16_t (*mem_read16)(CPU<W>&, address_type<W> addr);
		uint32_t (*mem_read32)(CPU<W>&, address_type<W> addr);
		uint64_t (*mem_read64)(CPU<W>&, address_type<W> addr);
		void (*mem_write8) (CPU<W>&, address_type<W> addr, uint8_t);
		void (*mem_write16)(CPU<W>&, address_type<W> addr, uint16_t);
		void (*mem_write32)(CPU<W>&, address_type<W> addr, uint32_t);
		void (*mem_write64)(CPU<W>&, address_type<W> addr, uint64_t);
		void (*jump)(CPU<W>&, address_type<W>);
		void (*increment_counter)(CPU<W>&, uint64_t);
		void (*trigger_exception)(CPU<W>&, int);
	};
}
