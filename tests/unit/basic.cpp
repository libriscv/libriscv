#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libriscv/machine.hpp>
extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2 -static", bool cpp = false);
static const uint64_t MAX_MEMORY = 8ul << 20; /* 8MB */
static const uint64_t MAX_INSTRUCTIONS = 10'000'000ul;
using namespace riscv;

TEST_CASE("Instantiate machine", "[Instantiate]")
{
	const auto binary = build_and_load(R"M(
	int main() {
		return 666;
	})M");
	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };

	// The stack is set to begin under the program area,
	// and the RISC-V toolchain is weird and starts at 0x10000.
	// libriscv will counteract this to make sure the stack can be
	// populated with runtime environment stuff, like program arguments,
	// and will put the stack at the end of the address space.
	REQUIRE(machine.memory.stack_initial() == ~(uint64_t)0 - 0xFFF);
	// The starting address is somewhere in the program area
	REQUIRE(machine.memory.start_address() > 0x10000);
}

TEST_CASE("Instantiate machine using shared ELF", "[Instantiate]")
{
	REQUIRE_THROWS([] {
		const auto binary = build_and_load(R"M(
		int main() {
			return 666;
		})M", "-shared");
		riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
		machine.simulate(MAX_INSTRUCTIONS);
	}());
}

TEST_CASE("Execute minimal machine", "[Minimal]")
{
	const auto binary = build_and_load(R"M(
	__asm__(".global _start\n"
	".section .text\n"
	"_start:\n"
	"	li a0, 666\n"
	"	li a7, 1\n"
	"	ecall\n");
	)M", "-static -ffreestanding -nostartfiles");
	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	machine.install_syscall_handler(1,
		[] (auto& machine) { machine.stop(); });
	machine.simulate(10);
	REQUIRE(machine.return_value<int>() == 666);
}

TEST_CASE("Execution timeout", "[Minimal]")
{
	const auto binary = build_and_load(R"M(
	__asm__(".global _start\n"
	".section .text\n"
	"_start:\n"
	"	j _start\n");
	)M", "-static -ffreestanding -nostartfiles");
	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	// Simulate 250k instructions before giving up
	REQUIRE_THROWS_WITH([&] {
		machine.simulate(250'000);
	}(), Catch::Matchers::ContainsSubstring("limit reached"));
}

TEST_CASE("Verify program arguments and environment", "[Runtime]")
{
	const auto binary = build_and_load(R"M(
	#include <string.h>
	extern char* getenv(char*);
	int main(int argc, char** argv) {
		if (strcmp(argv[0], "program") != 0)
			return -1;
		if (strcmp(argv[1], "this is a test") != 0)
			return -1;
		return 666;
	})M");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	// We need to install Linux system calls for maximum gucciness
	machine.setup_linux_syscalls();
	// We need to create a Linux environment for runtimes to work well
	machine.setup_linux(
		{"program", "this is a test"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	// Run for at most X instructions before giving up
	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(machine.return_value<int>() == 666);
}

TEST_CASE("Catch output from write system call", "[Output]")
{
	struct State {
		bool output_is_hello_world = false;
	} state;
	const auto binary = build_and_load(R"M(
	extern long write(int, const void*, unsigned long);
	int main() {
		write(1, "Hello World!", 12);
		return 666;
	})M");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	// We need to install Linux system calls for maximum gucciness
	machine.setup_linux_syscalls();
	// We need to create a Linux environment for runtimes to work well
	machine.setup_linux(
		{"basic"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	machine.set_userdata(&state);
	machine.set_printer([] (const auto& m, const char* data, size_t size) {
		auto* state = m.template get_userdata<State> ();
		std::string text{data, data + size};
		state->output_is_hello_world = (text == "Hello World!");
	});
	// Run for at most X instructions before giving up
	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(machine.return_value<int>() == 666);

	// We require that the write system call forwarded to the printer
	// and the data matched 'Hello World!'.
	REQUIRE(state.output_is_hello_world);
}

TEST_CASE("Calculate fib(50)", "[Compute]")
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

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	// We need to install Linux system calls for maximum gucciness
	machine.setup_linux_syscalls();
	// We need to create a Linux environment for runtimes to work well
	machine.setup_linux(
		{"basic", "50"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});
	// Run for at most X instructions before giving up
	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(machine.return_value<long>() == 12586269025L);
}
