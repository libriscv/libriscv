#include <libriscv/machine.hpp>
#include <cassert>
using namespace riscv;

void test_rv32c()
{
	const uint32_t memory = 65536;
	riscv::Machine<riscv::RISCV32> m { {}, memory };


}
