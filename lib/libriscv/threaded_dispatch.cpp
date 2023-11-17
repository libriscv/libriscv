#include "common.hpp"
#define DISPATCH_MODE_THREADED
#define DISPATCH_ATTR RISCV_HOT_PATH()
#define DISPATCH_FUNC simulate_threaded

#define EXECUTE_INSTR()      \
	if constexpr (FUZZING) { \
		if (UNLIKELY(decoder->get_bytecode() >= BYTECODES_MAX)) \
			abort();         \
	}                        \
	goto *computed_opcode[decoder->get_bytecode()];

#include "cpu_dispatch.cpp"

namespace riscv
{
	template <int W>
	void CPU<W>::simulate(uint64_t imax)
	{
		simulate_threaded(imax);
	}

	template struct CPU<4>;
	template struct CPU<8>;
	INSTANTIATE_128_IF_ENABLED(CPU);
} // riscv
