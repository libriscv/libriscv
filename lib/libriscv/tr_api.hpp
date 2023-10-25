#pragma once
#include <cstdint>

namespace riscv {
	template <int W>
	struct CallbackTable {
		const void* (*mem_read)(CPU<W>&, address_type<W> addr);
		void* (*mem_write) (CPU<W>&, address_type<W> addr);
		void (*vec_load)(CPU<W>&, int vd, address_type<W> addr);
		void (*vec_store) (CPU<W>&, address_type<W> addr, int vd);
		int  (*syscall)(CPU<W>&, address_type<W>);
		void (*ebreak)(CPU<W>&);
		void (*system)(CPU<W>&, uint32_t);
		void (*execute)(CPU<W>&, uint32_t);
		void (*trigger_exception)(CPU<W>&, int);
		float  (*sqrtf32)(float);
		double (*sqrtf64)(double);
	};
}
