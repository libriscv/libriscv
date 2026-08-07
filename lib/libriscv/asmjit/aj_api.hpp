#pragma once
#include "../types.hpp"

namespace riscv
{
	template <int W> struct CPU;

	// Host helpers callable from generated code. Every entry is noexcept:
	// a C++ exception must never unwind through an asmjit frame, because
	// asmjit-generated frames have no unwind information.
	//
	// These are the slow path only: with a flat memory arena the common case is
	// inlined, and a helper runs when the address falls outside the arena, which
	// means virtual paging, an MMIO-ish mapping, or a genuine protection fault.
	// Each takes the guest PC of the access so that a fault is reported against
	// the right instruction, and each already applies the sign- or zero-extension
	// that the RISC-V load requires, so the caller can drop the result straight
	// into the destination register.
	//
	// On a fault the helper records the exception, zeroes AjState::max_counter and
	// stops the machine; generated code checks max_counter after every helper call
	// and leaves the region immediately.
	template <int W>
	struct AjCallbacks
	{
		using addr_t = address_type<W>;

		addr_t (*load_i8) (CPU<W>&, AjState<W>*, addr_t addr, addr_t pc) noexcept;
		addr_t (*load_u8) (CPU<W>&, AjState<W>*, addr_t addr, addr_t pc) noexcept;
		addr_t (*load_i16)(CPU<W>&, AjState<W>*, addr_t addr, addr_t pc) noexcept;
		addr_t (*load_u16)(CPU<W>&, AjState<W>*, addr_t addr, addr_t pc) noexcept;
		addr_t (*load_i32)(CPU<W>&, AjState<W>*, addr_t addr, addr_t pc) noexcept;

		void (*store_8) (CPU<W>&, AjState<W>*, addr_t addr, addr_t value, addr_t pc) noexcept;
		void (*store_16)(CPU<W>&, AjState<W>*, addr_t addr, addr_t value, addr_t pc) noexcept;
		void (*store_32)(CPU<W>&, AjState<W>*, addr_t addr, addr_t value, addr_t pc) noexcept;
	};

	template <int W>
	const AjCallbacks<W>& aj_callbacks() noexcept;
}
