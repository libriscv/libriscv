#pragma once
#include <cstdint>

namespace riscv {
	template <int W>
	using syscall_t = void(*)(Machine<W>&);

	template <int W>
	struct CallbackTable {
		address_type<W> (*mem_read)(CPU<W>&, address_type<W> addr, unsigned size);
		void (*mem_write) (CPU<W>&, address_type<W> addr, address_type<W> value, unsigned size);
		void (*vec_load)(CPU<W>&, int vd, address_type<W> addr);
		void (*vec_store) (CPU<W>&, address_type<W> addr, int vd);
		syscall_t<W>* syscalls;
		int  (*system_call)(CPU<W>&, address_type<W>, uint64_t, uint64_t, int);
		void (*unknown_syscall)(CPU<W>&, address_type<W>);
		int  (*system)(CPU<W>&, uint32_t);
		unsigned (*execute)(CPU<W>&, uint32_t);
		unsigned (*execute_handler)(CPU<W>&, uint32_t, void(*)(CPU<W>&, union rv32i_instruction));
		void (**handlers)(CPU<W>&, uint32_t);
		void (*trigger_exception)(CPU<W>&, address_type<W>, int);
		void (*trace)(CPU<W>&, const char*, address_type<W>, uint32_t);
		float  (*sqrtf32)(float);
		double (*sqrtf64)(double);
		int (*clz) (uint32_t);
		int (*clzl) (uint64_t);
		int (*ctz) (uint32_t);
		int (*ctzl) (uint64_t);
		int (*cpop) (uint32_t);
		int (*cpopl) (uint64_t);
	};
}
