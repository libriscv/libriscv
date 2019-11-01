#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace riscv
{
	struct RV32I;
	struct RV64I;
	template <int W> struct CPU;

	template <int N>
	using address_type = typename std::conditional<(N == 4), uint32_t, uint64_t>::type;

	template <int N>
	using isa_type = typename std::conditional<(N == 4), RV32I, RV64I>::type;

	using interrupt_t = uint8_t;

	enum exceptions
	{
		DEBUG_INTERRUPT = 0,
		ILLEGAL_OPCODE,
		ILLEGAL_OPERATION,
		PROTECTION_FAULT,
		MISALIGNED_INSTRUCTION,
		UNIMPLEMENTED_INSTRUCTION,
		UNIMPLEMENTED_SYSCALL,
	};

	template <int W>
	struct Instruction {
		using isa_t     = isa_type<W>;              // 32- or 64-bit architecture
		using format_t  = typename isa_t::format_t; // one machine instruction

		using handler_t = void (*)(CPU<W>&, format_t);
		using printer_t = int  (*)(char*, size_t, CPU<W>&, format_t);

		const handler_t handler; // callback for executing one instruction
		const printer_t printer; // callback for logging one instruction
	};

	static_assert(DEBUG_INTERRUPT == 0, "Debug interrupt must be zero");

#ifdef RISCV_DEBUG
	static constexpr bool riscv_debug_enabled = true;
#else
	static constexpr bool riscv_debug_enabled = false;
#endif
	static constexpr bool verbose_machine = false;
}
