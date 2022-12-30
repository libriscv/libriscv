#define DISPATCH_MODE_SWITCH_BASED
#define DISPATCH_ATTR RISCV_HOT_PATH()
#define DISPATCH_FUNC simulate_bytecode

#define EXECUTE_INSTR() \
	break;

#include "cpu_dispatch.cpp"

namespace riscv
{
	template <int W>
	void CPU<W>::simulate(uint64_t imax)
	{
		simulate_bytecode(imax);
	}

	template struct CPU<4>;
	template struct CPU<8>;
	INSTANTIATE_128_IF_ENABLED(CPU);
} // riscv
