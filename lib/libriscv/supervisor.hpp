#pragma once
#include "types.hpp"

namespace riscv
{
    template <int W>
    struct Supervisor {
        using address_t = address_type<W>;

        void sret() {}

        address_t satp = 0x0;
        address_t mie;
        address_t mstatus = 0x0;
    };
} // riscv
