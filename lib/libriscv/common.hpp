#pragma once

#ifndef LIKELY
#define LIKELY(x) __builtin_expect((x), 1)
#endif
#ifndef UNLIKELY
#define UNLIKELY(x) __builtin_expect((x), 0)
#endif

namespace riscv
{
	static constexpr int EBREAK_SYSCALL = 0;

	// print information during machine creation
	extern bool verbose_machine;

#ifdef RISCV_DEBUG
	static constexpr bool memory_debug_enabled = true;
#else
	static constexpr bool memory_debug_enabled = false;
#endif
}
