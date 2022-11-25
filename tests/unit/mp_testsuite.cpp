#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libriscv/machine.hpp>
extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2 -static", bool cpp = false);
static const uint64_t MAX_INSTRUCTIONS = 10'000'000ul;
static const std::string cwd {SRCDIR};
using namespace riscv;
static uint64_t stack_base = 0x0;
static uint64_t stack_size = 1ul << 20;

static void install_multiprocessing_syscalls()
{
	Machine<RISCV64>::install_syscall_handler(1,
	[] (Machine<RISCV64>& machine) {
		auto [vcpus, stk, stksize] = machine.sysargs <unsigned, uint64_t, uint64_t> ();
		if (stk != 0x0 && stksize != 0) {
			// Use another stack
			machine.multiprocess(vcpus, MAX_INSTRUCTIONS, (uint64_t)stk, (uint64_t)stksize);
		} else {
			// Use current thread
			machine.multiprocess(vcpus, MAX_INSTRUCTIONS, stack_base, stack_size);
		}
		machine.set_result(0);
	});
	Machine<RISCV64>::install_syscall_handler(2,
	[] (Machine<RISCV64>& machine) {
		if (machine.cpu.cpu_id() == 0) {
			machine.set_result(machine.multiprocess_wait());
		} else {
			machine.stop();
		}
	});
	Machine<RISCV64>::install_syscall_handler(10,
	[] (Machine<RISCV64>& machine) {
		auto buffer = machine.sysarg<std::string>(0);
		printf(">>> Guest says: %s\n", buffer.c_str());
	});
}

TEST_CASE("Singleprocessing dot-product", "[Compute]")
{
	const auto binary = build_and_load(R"M(
	#include <cassert>
	#include "mp_testsuite.hpp"

	int main(int, char**) {
		initialize_work(mp_work);
		mp_work.workers = 1;

		multiprocessing_function<WORK_SIZE> (0, &mp_work);

		// Verify results
		assert(mp_work.final_sum() == WORK_SIZE);
		assert(mp_work.counter == 1);
		return mp_work.final_sum();
	})M", "-O0 -static -I" + cwd, true);

	Machine<RISCV64> machine { binary };
	machine.setup_linux_syscalls();
	install_multiprocessing_syscalls();
	machine.setup_linux(
		{"singleprocessing"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=groot"});

	// Discover stack
	stack_base = machine.memory.stack_initial() - stack_size;

	// Run for at most X instructions before giving up
	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(machine.return_value<long>() == 16384);
}

TEST_CASE("Multiprocessing (forked) dot-product", "[Compute]")
{
	const auto binary = build_and_load(R"M(
	#include <cassert>
	#include "mp_testsuite.hpp"

	int main()
	{
		initialize_work(mp_work);
		mp_work.workers = MP_WORKERS;

		// Fork this machine and wait until multiprocessing
		// ends, by calling multiprocess_wait() on all workers. Each
		// worker uses the current stack, copy-on-write.
		unsigned cpu = multiprocess(MP_WORKERS);
		if (cpu != 0) {
			multiprocessing_function<WORK_SIZE> (cpu-1, &mp_work);
		}
		long result = multiprocess_wait();
		assert(result == 0);

		// Verify results
		assert(mp_work.counter == MP_WORKERS);
		assert(mp_work.final_sum() == WORK_SIZE);
		return mp_work.final_sum();
	})M", "-O0 -static -I" + cwd, true);

	Machine<RISCV64> machine { binary };
	machine.setup_linux_syscalls();
	install_multiprocessing_syscalls();
	machine.setup_linux(
		{"multiprocessing"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=groot"});

	// Discover stack (XXX: Not ideal)
	stack_base = machine.memory.stack_initial() - stack_size;

	// Run for at most X instructions before giving up
	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(!machine.is_multiprocessing());

	REQUIRE(machine.return_value<long>() == 16384);
}

TEST_CASE("Multiprocessing dot-product forever", "[Compute]")
{
	const auto binary = build_and_load(R"M(
	#include <cassert>
	#include "mp_testsuite.hpp"

	int main()
	{
		initialize_work(mp_work);
		mp_work.workers = MP_WORKERS;

		// This will fail, because it never completes
		unsigned cpu = multiprocess(MP_WORKERS);
		if (cpu != 0x0) {
			while (true);
		}
		// Wait and stop workers here
		long result = multiprocess_wait();
		// Result will have bits set for each failing vCPU (except vCPU 0)
		assert(result == 0b11110);

		// Verify results
		assert(mp_work.final_sum() == 0);
		assert(mp_work.counter == 0);
		return mp_work.final_sum();
	})M", "-O0 -static -I" + cwd, true);

	Machine<RISCV64> machine { binary };
	machine.setup_linux_syscalls();
	install_multiprocessing_syscalls();
	machine.setup_linux(
		{"multiprocessing_forever"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=groot"});
	// Run for at most X instructions before giving up
	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(!machine.is_multiprocessing());

	REQUIRE(machine.return_value() == 0);
}
