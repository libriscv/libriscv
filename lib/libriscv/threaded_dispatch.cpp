#define DISPATCH_MODE_THREADED
#define DISPATCH_FUNC simulate_threaded

#define NEXT_INSTR()                  \
	if constexpr (compressed_enabled) \
		decoder += 2;                 \
	else                              \
		decoder += 1;                 \
	goto *computed_opcode[decoder->get_bytecode()];
#define NEXT_C_INSTR() \
	decoder += 1;      \
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
