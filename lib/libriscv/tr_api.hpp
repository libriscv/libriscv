#pragma once
#include <cstdint>

namespace riscv {
	template <int W>
	struct CallbackTable {
		const void* (*mem_read)(CPU<W>&, address_type<W> addr);
		void* (*mem_write) (CPU<W>&, address_type<W> addr);
		void (*jump)(CPU<W>&, address_type<W>);
		int  (*syscall)(CPU<W>&, address_type<W>);
		void (*stop)(CPU<W>&);
		void (*ebreak)(CPU<W>&);
		void (*system)(CPU<W>&, uint32_t);
		void (*execute)(CPU<W>&, uint32_t);
		void (*trigger_exception)(CPU<W>&, int);
		float  (*sqrtf32)(float);
		double (*sqrtf64)(double);
	};
}
