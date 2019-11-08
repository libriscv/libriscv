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

	#ifdef RISCV_DEBUG
		static constexpr bool verbose_machine = true;
		static constexpr bool memory_debug_enabled = true;
	#else
		static constexpr bool verbose_machine = false;
		static constexpr bool memory_debug_enabled = false;
	#endif
}
