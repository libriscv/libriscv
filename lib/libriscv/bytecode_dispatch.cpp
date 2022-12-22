#define DISPATCH_MODE_SWITCH_BASED
#define DISPATCH_FUNC simulate_bytecode

#define NEXT_INSTR()                  \
    if constexpr (compressed_enabled) \
        decoder += 2;                 \
    else                              \
        decoder += 1;                 \
    break;
#define NEXT_C_INSTR() \
    decoder += 1;      \
    break;

#include "cpu_dispatch.cpp"

namespace riscv
{

#ifndef RISCV_THREADED
    template <int W>
    void CPU<W>::simulate(uint64_t imax)
    {
        simulate_bytecode(imax);
    }
#endif

    template struct CPU<4>;
    template struct CPU<8>;
    template struct CPU<16>;
} // riscv
