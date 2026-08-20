#pragma once
#include "../types.hpp"

#include <asmjit/core.h>
#if !defined(ASMJIT_NO_UJIT)
#  include <asmjit/ujit.h>
#endif

// The emitter is written against asmjit's universal compiler (ujit), which
// covers x86-64 and AArch64 from a single source. asmjit defines exactly one
// of these two when it has a code generator for the host it is built on.
#if defined(ASMJIT_UJIT_X86) || defined(ASMJIT_UJIT_AARCH64)
#  define RISCV_ASMJIT_HAS_BACKEND 1
#else
#  define RISCV_ASMJIT_HAS_BACKEND 0
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
