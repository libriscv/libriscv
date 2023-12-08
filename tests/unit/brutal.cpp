#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libriscv/machine.hpp>
#include <libriscv/debug.hpp>
extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2 -static", bool cpp = false);
using namespace riscv;

/**
 * These tests are designed to be really brutal to support,
 * and most emulators will surely fail here.
*/

TEST_CASE("Calculate fib(50) slowly", "[Compute]")
{
	const auto binary = build_and_load(R"M(
	#include <stdlib.h>
	long fib(long n, long acc, long prev)
	{
		if (n < 1)
			return acc;
		else
			return fib(n - 1, prev + acc, acc);
	}
	long main(int argc, char** argv) {
		const long n = atoi(argv[1]);
		return fib(n, 0, 1);
	})M");

	riscv::Machine<RISCV64> machine { binary, { .use_memory_arena = false } };
	machine.setup_linux_syscalls();
	machine.setup_linux(
		{"brutal", "50"},
		{"LC_TYPE=C", "LC_ALL=C"});

	do {
		// No matter how many (or few) instructions we execute before exiting
		// simulation, we should be able to resume and complete the program normally.
		for (int step = 5; step < 105; step++) {
			riscv::Machine<RISCV64> fork { machine };
			do {
				fork.simulate<false>(step);
			} while (fork.instruction_limit_reached());
			REQUIRE(fork.return_value<long>() == 12586269025L);
		}
		machine.simulate<false>(100);
	} while (machine.instruction_limit_reached());
}
