#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libriscv/machine.hpp>
#include <libriscv/native_heap.hpp>
extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2 -static", bool cpp = false);
static const uint64_t MAX_INSTRUCTIONS = 10'000'000ul;
static const std::string cwd {SRCDIR};
using namespace riscv;

static const int HEAP_SYSCALLS_BASE	  = 470;
static const int MEMORY_SYSCALLS_BASE = 475;
static const int THREADS_SYSCALL_BASE = 490;

template <int W>
static void setup_native_system_calls(riscv::Machine<W>& machine)
{
	// Syscall-backed heap
	constexpr size_t heap_size = 65536;
	auto heap = machine.memory.mmap_allocate(heap_size);

	machine.setup_native_heap(HEAP_SYSCALLS_BASE, heap, heap_size);
	machine.setup_native_memory(MEMORY_SYSCALLS_BASE);
	machine.setup_native_threads(THREADS_SYSCALL_BASE);
}

TEST_CASE("Activate native helper syscalls", "[Native]")
{
	const auto binary = build_and_load(R"M(
	#include <stdlib.h>
	#include <stdio.h>
	int main(int argc, char** argv)
	{
		const char *hello = (const char*)atol(argv[1]);
		printf("%s\n", hello);
		return 666;
	})M");

	riscv::Machine<RISCV64> machine { binary };
	machine.setup_linux_syscalls();

	setup_native_system_calls(machine);

	// Allocate string on heap
	static const std::string hello = "Hello World!";
	auto addr = machine.arena().malloc(64);
	machine.copy_to_guest(addr, hello.data(), hello.size()+1);

	// Pass string address to guest as main argument
	machine.setup_linux(
		{"native", std::to_string(addr)},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	// Catch output from machine
	struct State {
		bool output_is_hello_world = false;
	} state;

	machine.set_userdata(&state);
	machine.set_printer([] (const auto& m, const char* data, size_t size) {
		auto* state = m.template get_userdata<State> ();
		std::string text{data, data + size};
		// musl writev:
		state->output_is_hello_world = state->output_is_hello_world || (text == "Hello World!");
		// glibc write:
		state->output_is_hello_world = state->output_is_hello_world || (text == "Hello World!\n");
	});

	// Run simulation
	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(machine.return_value() == 666);
	REQUIRE(state.output_is_hello_world);
}

TEST_CASE("Use native helper syscalls", "[Native]")
{
	const auto binary = build_and_load(R"M(
	#include <include/native_libc.h>
	#include <stdlib.h>
	#include <stdio.h>
	int main()
	{
		char* hello = malloc(13);
		memcpy(hello, "Hello World!", 13);
		printf("%s\n", hello);
		return 666;
	})M", "-O2 -static -I" + cwd);

	riscv::Machine<RISCV64> machine { binary };

	setup_native_system_calls(machine);

	machine.setup_linux_syscalls();
	machine.setup_linux(
		{"native"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	// Catch output from machine
	struct State {
		bool output_is_hello_world = false;
	} state;

	machine.set_userdata(&state);
	machine.set_printer([] (const auto& m, const char* data, size_t size) {
		auto* state = m.template get_userdata<State> ();
		std::string text{data, data + size};
		// musl writev:
		state->output_is_hello_world = state->output_is_hello_world || (text == "Hello World!");
		// glibc write:
		state->output_is_hello_world = state->output_is_hello_world || (text == "Hello World!\n");
	});

	// Run simulation
	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(machine.return_value() == 666);
	REQUIRE(state.output_is_hello_world);
}

TEST_CASE("Free unknown causes exception", "[Native]")
{
	const auto binary = build_and_load(R"M(
	#include <include/native_libc.h>
	int main()
	{
		free((void *)0x1234);
		return 666;
	})M", "-O2 -static -I" + cwd);

	riscv::Machine<RISCV64> machine { binary };
	setup_native_system_calls(machine);

	machine.setup_linux_syscalls();
	machine.setup_linux(
		{"native"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	bool error = false;
	try {
		machine.simulate(MAX_INSTRUCTIONS);
	} catch (const std::exception& e) {
		// Libtcc does not forward the real exception (instead throws a generic SYSTEM_CALL_FAILED)
		if constexpr (!libtcc_enabled)
			REQUIRE(std::string(e.what()) == "Possible double-free for freed pointer");
		error = true;
	}
	REQUIRE(error);
}
