#pragma once

#ifndef LIKELY
#define LIKELY(x) __builtin_expect((x), 1)
#endif
#ifndef UNLIKELY
#define UNLIKELY(x) __builtin_expect((x), 0)
#endif

#ifndef SYSCALL_EBREAK_NR
#define SYSCALL_EBREAK_NR    0
#endif

#ifndef MEMORY_TRAPS_ENABLED
# ifdef RISCV_DEBUG
#  define MEMORY_TRAPS_ENABLED
# endif
#endif

namespace riscv
{
	static constexpr int SYSCALL_EBREAK = SYSCALL_EBREAK_NR;

	// print information during machine creation
	extern bool verbose_machine;

#ifdef MEMORY_TRAPS_ENABLED
	static constexpr bool memory_debug_enabled = true;
#else
	static constexpr bool memory_debug_enabled = false;
#endif

	// assert on misaligned reads/writes
	static constexpr bool memory_alignment_check = false;
}
