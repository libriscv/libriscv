#define DISPATCH_MODE_THREADED
#define DISPATCH_FUNC simulate_threaded

#include "cpu_dispatch.cpp"

namespace riscv
{

#ifdef RISCV_THREADED
    template <int W>
    void CPU<W>::simulate(uint64_t imax)
    {
        simulate_threaded(imax);
    }
#endif

    template struct CPU<4>;
    template struct CPU<8>;
    template struct CPU<16>;
} // riscv
