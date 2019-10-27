#pragma once
#include <libriscv/machine.hpp>

template<int W>
void prepare_linux(riscv::Machine<W>&, const std::vector<std::string>&);
