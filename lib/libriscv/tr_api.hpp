#pragma once
#include <cstdint>

namespace riscv {
	template <int W>
	using syscall_t = void(*)(Machine<W>&);

	template <int W>
	struct CallbackTable {
		uint8_t (*mem_read8)(CPU<W>&, address_type<W> addr);
		uint16_t (*mem_read16)(CPU<W>&, address_type<W> addr);
		uint32_t (*mem_read32)(CPU<W>&, address_type<W> addr);
		uint64_t (*mem_read64)(CPU<W>&, address_type<W> addr);
		void (*mem_write8)(CPU<W>&, address_type<W> addr, uint8_t value);
		void (*mem_write16)(CPU<W>&, address_type<W> addr, uint16_t value);
		void (*mem_write32)(CPU<W>&, address_type<W> addr, uint32_t value);
		void (*mem_write64)(CPU<W>&, address_type<W> addr, uint64_t value);
		void (*vec_load)(CPU<W>&, int vd, address_type<W> addr);
		void (*vec_store) (CPU<W>&, address_type<W> addr, int vd);
		syscall_t<W>* syscalls;
		uint64_t (*system_call)(CPU<W>&, address_type<W>, uint64_t, uint64_t, int);
		void (*unknown_syscall)(CPU<W>&, address_type<W>);
		int  (*system)(CPU<W>&, uint32_t);
		unsigned (*execute)(CPU<W>&, uint32_t);
		unsigned (*execute_handler)(CPU<W>&, uint32_t, void(*)(CPU<W>&, union rv32i_instruction));
		void (**handlers)(CPU<W>&, uint32_t);
		void (*trigger_exception)(CPU<W>&, address_type<W>, int);
		void (*trace)(CPU<W>&, const char*, address_type<W>, uint32_t);
		float  (*sqrtf32)(float);
		double (*sqrtf64)(double);
		// Fused multiply-add (single rounding) — required by RISC-V spec
		// §11.6 for FMADD/FMSUB/FNMADD/FNMSUB. TCC compiles emitted
		// `a*b+c` as two roundings; emitted code routes through these
		// entries to std::fma for correct behavior. Must match the
		// `static struct CallbackTable` in tr_api.cpp field-for-field.
		float  (*fmaf32)(float, float, float);
		double (*fmaf64)(double, double, double);
		// FMIN/FMAX with RISC-V's -0.0 < +0.0 convention (std::fmin/fmax
		// leaves the ±0 case implementation-defined; emitted code would
		// otherwise return host-dependent signs).
		float  (*fmin32_rv)(float, float);
		float  (*fmax32_rv)(float, float);
		double (*fmin64_rv)(double, double);
		double (*fmax64_rv)(double, double);
		int (*clz) (uint32_t);
		int (*clzl) (uint64_t);
		int (*ctz) (uint32_t);
		int (*ctzl) (uint64_t);
		int (*cpop) (uint32_t);
		int (*cpopl) (uint64_t);
	};
}
