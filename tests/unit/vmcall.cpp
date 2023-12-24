#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libriscv/machine.hpp>
extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2 -static -Wl,--undefined=hello", bool cpp = false);
static const uint64_t MAX_MEMORY = 8ul << 20; /* 8MB */
static const uint64_t MAX_INSTRUCTIONS = 10'000'000ul;
using namespace riscv;

TEST_CASE("VM function call", "[VMCall]")
{
	struct State {
		bool output_is_hello_world = false;
	} state;
	const auto binary = build_and_load(R"M(
	extern long write(int, const void*, unsigned long);
	extern void hello() {
		write(1, "Hello World!", 12);
	}

	int main() {
		return 666;
	})M");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	// We need to install Linux system calls for maximum gucciness
	machine.setup_linux_syscalls();
	// We need to create a Linux environment for runtimes to work well
	machine.setup_linux(
		{"vmcall"},
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
	REQUIRE(!state.output_is_hello_world);

	const auto hello_address = machine.address_of("hello");
	REQUIRE(hello_address != 0x0);

	// Execute guest function
	machine.vmcall(hello_address);

	// Now hello world should have been printed
	REQUIRE(state.output_is_hello_world);
}

TEST_CASE("VM function call in fork", "[VMCall]")
{
	// The global variable 'value' should get
	// forked as value=1. We assert this, then
	// we set value=0. New forks should continue
	// to see value=1 as they are forked from the
	// main VM where value is still 0.
	const auto binary = build_and_load(R"M(
	#include <assert.h>
	#include <string.h>
	extern long write(int, const void*, unsigned long);
	static int value = 0;

	extern void hello() {
		assert(value == 1);
		value = 0;
		write(1, "Hello World!", 12);
	}

	extern int str(const char *arg) {
		assert(strcmp(arg, "Hello") == 0);
		return 1;
	}

	struct Data {
		int val1;
		int val2;
		float f1;
	};
	extern int structs(struct Data *data) {
		assert(data->val1 == 1);
		assert(data->val2 == 2);
		assert(data->f1 == 3.0f);
		return 2;
	}

	extern int ints(long i1, long i2, long i3) {
		assert(i1 == 123);
		assert(i2 == 456);
		assert(i3 == 456);
		return 3;
	}

	int main() {
		value = 1;
		return 666;
	})M");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	machine.setup_linux_syscalls();
	machine.setup_linux(
		{"vmcall"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	machine.simulate(MAX_INSTRUCTIONS);
	REQUIRE(machine.return_value<int>() == 666);

	// Test many forks
	for (size_t i = 0; i < 10; i++)
	{
		riscv::Machine<RISCV64> fork { machine, {
			.use_memory_arena = false
		} };
		REQUIRE(fork.memory.uses_flat_memory_arena() == false);

		fork.set_printer([] (const auto&, const char* data, size_t size) {
			std::string text{data, data + size};
			REQUIRE(text == "Hello World!");
		});

		const auto hello_address = fork.address_of("hello");
		REQUIRE(hello_address != 0x0);

		// Execute guest function
		fork.vmcall(hello_address);

		int res1 = fork.vmcall("str", "Hello");
		REQUIRE(res1 == 1);

		res1 = fork.vmcall("str", std::string("Hello"));
		REQUIRE(res1 == 1);

		std::string hello = "Hello";
		const std::string& ref = hello;

		res1 = fork.vmcall("str", ref);
		REQUIRE(res1 == 1);

		struct {
			int v1 = 1;
			int v2 = 2;
			float f1 = 3.0f;
		} data;
		int res2 = fork.vmcall("structs", data);
		REQUIRE(res2 == 2);

		long intval = 456;
		long& intref = intval;

		int res3 = fork.vmcall("ints", 123L, intref, (long&&)intref);
		REQUIRE(res3 == 3);

		// XXX: Binary translation currently "remembers" that arena
		// was enabled, and will not disable it for the fork.
		if constexpr (riscv::flat_readwrite_arena && riscv::binary_translation_enabled)
			return;
	}
}

TEST_CASE("VM call and preemption", "[VMCall]")
{
	struct State {
		bool output_is_hello_world = false;
	} state;
	const auto binary = build_and_load(R"M(
	extern long write(int, const void*, unsigned long);
	long syscall1(long n, long arg0) {
		register long a0 __asm__("a0") = arg0;
		register long syscall_id __asm__("a7") = n;

		__asm__ volatile ("scall" : "+r"(a0) : "r"(syscall_id));

		return a0;
	}

	extern long start() {
		syscall1(500, 1234567);
		return 1;
	}
	extern void preempt(int arg) {
		write(1, "Hello World!", arg);
	}

	int main() {
		syscall1(500, 1234567);
		return 666;
	})M");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	machine.setup_linux_syscalls();
	machine.setup_linux(
		{"vmcall"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	machine.set_userdata(&state);
	machine.set_printer([] (const auto& m, const char* data, size_t size) {
		auto* state = m.template get_userdata<State> ();
		std::string text{data, data + size};
		state->output_is_hello_world = (text == "Hello World!");
	});

	machine.install_syscall_handler(500,
	[] (auto& machine) {
		auto [arg0] = machine.template sysargs <int> ();
		REQUIRE(arg0 == 1234567);

		const auto func = machine.address_of("preempt");
		REQUIRE(func != 0x0);

		machine.preempt(15'000ull, func, strlen("Hello World!"));
	});

	REQUIRE(!state.output_is_hello_world);

	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(state.output_is_hello_world);
	REQUIRE(machine.return_value<int>() == 666);

	for (int i = 0; i < 10; i++)
	{
		state.output_is_hello_world = false;

		const auto func = machine.address_of("start");
		REQUIRE(func != 0x0);

		// Execute guest function
		machine.vmcall<15'000ull>(func);
		REQUIRE(machine.return_value<int>() == 1);

		// Now hello world should have been printed
		REQUIRE(state.output_is_hello_world);
	}
}

TEST_CASE("VM call and STOP instruction", "[VMCall]")
{
	struct State {
		bool output_is_hello_world = false;
	} state;
	const auto binary = build_and_load(R"M(
	extern long write(int, const void*, unsigned long);
	long syscall1(long n, long arg0) {
		register long a0 __asm__("a0") = arg0;
		register long syscall_id __asm__("a7") = n;

		__asm__ volatile ("scall" : "+r"(a0) : "r"(syscall_id));

		return a0;
	}
	void return_fast1(long retval)
	{
		register long a0 __asm__("a0") = retval;

		__asm__ volatile (".insn i SYSTEM, 0, x0, x0, 0x7ff" :: "r"(a0));
		__builtin_unreachable();
	}

	extern long start() {
		syscall1(500, 1234567);
		return_fast1(1234);
		return 5678;
	}
	extern long preempt(int arg) {
		write(1, "Hello World!", arg);
		return_fast1(777);
	}

	int main() {
		syscall1(500, 1234567);
		return_fast1(777);
		return 666;
	})M");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	machine.setup_linux_syscalls();
	machine.setup_linux(
		{"vmcall"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	machine.set_userdata(&state);
	machine.set_printer([] (const auto& m, const char* data, size_t size) {
		auto* state = m.template get_userdata<State> ();
		std::string text{data, data + size};
		state->output_is_hello_world = (text == "Hello World!");
	});

	machine.install_syscall_handler(500,
	[] (auto& machine) {
		auto [arg0] = machine.template sysargs <int> ();
		REQUIRE(arg0 == 1234567);

		const auto func = machine.address_of("preempt");
		REQUIRE(func != 0x0);

		auto result = machine.preempt(15'000ull, func, strlen("Hello World!"));
		REQUIRE(result == 777);
	});

	REQUIRE(!state.output_is_hello_world);

	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(state.output_is_hello_world);
	REQUIRE(machine.return_value<int>() == 777);

	for (int i = 0; i < 10; i++)
	{
		state.output_is_hello_world = false;

		const auto func = machine.address_of("start");
		REQUIRE(func != 0x0);

		// Execute guest function
		machine.vmcall<15'000ull>(func);
		REQUIRE(machine.return_value<int>() == 1234);

		// Now hello world should have been printed
		REQUIRE(state.output_is_hello_world);
	}
}
