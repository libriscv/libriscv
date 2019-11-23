#pragma once
#include <libriscv/machine.hpp>

template<int W>
void prepare_linux(riscv::Machine<W>& machine,
					const std::vector<std::string>& args,
					const std::vector<std::string>& env);
