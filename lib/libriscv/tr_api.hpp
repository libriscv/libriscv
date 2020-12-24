#pragma once
#include <cstdint>

#ifdef RISCV_TRANSLATION_DYLIB
#ifndef LIKELY
#define LIKELY(x) __builtin_expect((x), 1)
#endif
#ifndef UNLIKELY
#define UNLIKELY(x) __builtin_expect((x), 0)
#endif
#endif

namespace riscv {
#ifdef RISCV_TRANSLATION_DYLIB
	// We know what we are building for
#if RISCV_TRANSLATION_DYLIB == 4
	using address_t = uint32_t;
	using saddress_t = int32_t;
#else
	using address_t = uint64_t;
	using saddress_t = int64_t;
#endif

	// Thin variant of CPU for higher compilation speed
	struct ThinCPU {
		static constexpr int W = RISCV_TRANSLATION_DYLIB;
		using format_t  = union rv32i_instruction;  // one machine instruction

		void set_pc(address_t addr) { m_regs.pc = addr; }
		address_t& reg(uint32_t idx) { return m_regs.regs[idx]; }
		const address_t& reg(uint32_t idx) const { return m_regs.regs[idx]; }

	private:
		using register_t = address_t;
		struct {
			address_t  pc;
			register_t regs[32];
		} m_regs;
	};
	struct CallbackTable {
		uint8_t  (*mem_read8)(ThinCPU&, address_t addr);
		uint16_t (*mem_read16)(ThinCPU&, address_t addr);
		uint32_t (*mem_read32)(ThinCPU&, address_t addr);
		uint64_t (*mem_read64)(ThinCPU&, address_t addr);
		void (*mem_write8) (ThinCPU&, address_t addr, uint8_t);
		void (*mem_write16)(ThinCPU&, address_t addr, uint16_t);
		void (*mem_write32)(ThinCPU&, address_t addr, uint32_t);
		void (*mem_write64)(ThinCPU&, address_t addr, uint64_t);
		void (*jump)(ThinCPU&, address_t);
		void (*increment_counter)(ThinCPU&, uint64_t);
		void (*trigger_exception)(ThinCPU&, int);
	};
#else
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
#endif
}
