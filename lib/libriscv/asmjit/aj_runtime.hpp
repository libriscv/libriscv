#pragma once
#include "../types.hpp"

#if defined(__x86_64__) || defined(_M_X64)
#  define RISCV_ASMJIT_HAS_BACKEND 1
#  include <asmjit/x86.h>
#else
#  define RISCV_ASMJIT_HAS_BACKEND 0
#  include <asmjit/core.h>
#endif

namespace riscv
{
	// Owns all machine code emitted for one execute segment.
	// Held by shared_ptr from DecodedExecuteSegment so that code outlives
	// forks and shared execute segments.
	struct AjCode
	{
		asmjit::JitRuntime rt;

		AjCode() = default;
		AjCode(const AjCode&) = delete;
		AjCode& operator=(const AjCode&) = delete;
		// ~JitRuntime releases every function it allocated.
	};
}
