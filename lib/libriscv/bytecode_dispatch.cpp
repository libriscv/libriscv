#include "common.hpp"
#include "internal_common.hpp"
#define DISPATCH_MODE_SWITCH_BASED
#define DISPATCH_ATTR RISCV_HOT_PATH()
#define DISPATCH_FUNC simulate_bytecode

#define EXECUTE_INSTR() \
	continue;
#define UNUSED_FUNCTION() \
	RISCV_UNREACHABLE(); break;

#include "cpu_dispatch.cpp"

#include "cpu_inaccurate_dispatch.cpp"

namespace riscv
{
	INSTANTIATE_32_IF_ENABLED(CPU);
	INSTANTIATE_64_IF_ENABLED(CPU);
	INSTANTIATE_128_IF_ENABLED(CPU);
} // riscv
