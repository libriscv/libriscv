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
		// RV64 only: LWU zero-extends where LW sign-extends, and LD is full width.
		addr_t (*load_u32)(CPU<W>&, AjState<W>*, addr_t addr, addr_t pc) noexcept;
		addr_t (*load_i64)(CPU<W>&, AjState<W>*, addr_t addr, addr_t pc) noexcept;

		void (*store_8) (CPU<W>&, AjState<W>*, addr_t addr, addr_t value, addr_t pc) noexcept;
		void (*store_16)(CPU<W>&, AjState<W>*, addr_t addr, addr_t value, addr_t pc) noexcept;
		void (*store_32)(CPU<W>&, AjState<W>*, addr_t addr, addr_t value, addr_t pc) noexcept;
		void (*store_64)(CPU<W>&, AjState<W>*, addr_t addr, addr_t value, addr_t pc) noexcept;   // RV64 only

		// --- F/D extension -------------------------------------------------
		// FLD and FSD move 64 bits on RV32 too, so the FP accessors carry a
		// fixed uint64_t payload rather than the guest's register width. The
		// value is the raw bit pattern of the f-register, never a host double:
		// a NaN must survive the round trip unchanged.
		uint64_t (*load_fl) (CPU<W>&, AjState<W>*, addr_t addr, addr_t pc) noexcept;  // FLW
		uint64_t (*load_dbl)(CPU<W>&, AjState<W>*, addr_t addr, addr_t pc) noexcept;  // FLD
		void (*store_fl) (CPU<W>&, AjState<W>*, addr_t addr, uint64_t value, addr_t pc) noexcept;
		void (*store_dbl)(CPU<W>&, AjState<W>*, addr_t addr, uint64_t value, addr_t pc) noexcept;

		// FMIN/FMAX: RISC-V orders -0.0 below +0.0 and canonicalizes a pair of
		// NaNs, neither of which any host min/max instruction does.
		float  (*fmin32)(float, float) noexcept;
		float  (*fmax32)(float, float) noexcept;
		double (*fmin64)(double, double) noexcept;
		double (*fmax64)(double, double) noexcept;

		// FCLASS reads the raw exponent and mantissa fields, so it takes bits.
		addr_t (*fclass32)(uint32_t bits) noexcept;
		addr_t (*fclass64)(uint64_t bits) noexcept;

		// Float to integer. Every host conversion instruction disagrees with
		// RISC-V about NaN and about overflow, and the rounding mode is part of
		// the instruction, so these stay helper calls. `rm` is the raw funct3;
		// the DYN encoding (0x7) is resolved against the CPU's frm here.
		addr_t (*fcvt_w_s) (CPU<W>&, float,  uint32_t rm) noexcept;
		addr_t (*fcvt_wu_s)(CPU<W>&, float,  uint32_t rm) noexcept;
		addr_t (*fcvt_l_s) (CPU<W>&, float,  uint32_t rm) noexcept;   // RV64 only
		addr_t (*fcvt_lu_s)(CPU<W>&, float,  uint32_t rm) noexcept;   // RV64 only
		addr_t (*fcvt_w_d) (CPU<W>&, double, uint32_t rm) noexcept;
		addr_t (*fcvt_wu_d)(CPU<W>&, double, uint32_t rm) noexcept;
		addr_t (*fcvt_l_d) (CPU<W>&, double, uint32_t rm) noexcept;   // RV64 only
		addr_t (*fcvt_lu_d)(CPU<W>&, double, uint32_t rm) noexcept;   // RV64 only

		// Executes one instruction on the interpreter's handler: used for the
		// atomics, the CSRs, CBO.ZERO, the vector unit and the Zfa/Zfhmin
		// encodings with no compact host sequence. `handler` is the
		// instruction_handler<W> for this encoding, resolved at translation
		// time. PC is stored to the CPU first, as traps report against it.
		void (*execute)(CPU<W>&, AjState<W>*, uint32_t instr, addr_t pc,
			const void* handler) noexcept;

		// Zbc's carry-less multiplies, for a host without PCLMULQDQ or an
		// equivalent. Each selects a window of the same 2*XLEN-bit product.
		addr_t (*clmul) (addr_t, addr_t) noexcept;
		addr_t (*clmulh)(addr_t, addr_t) noexcept;
		addr_t (*clmulr)(addr_t, addr_t) noexcept;

		// Integer to float. Only the 64-bit unsigned sources need a helper:
		// no host has an instruction for them, and the signed forms are one
		// instruction each and are emitted inline.
		float  (*fcvt_s_lu)(uint64_t) noexcept;                       // RV64 only
		double (*fcvt_d_lu)(uint64_t) noexcept;                       // RV64 only
	};

	template <int W>
	const AjCallbacks<W>& aj_callbacks() noexcept;
}
