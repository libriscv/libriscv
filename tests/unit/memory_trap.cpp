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
	extern void hello_write() {
		*(long *)0xF0000000 = 1234;
	}
	extern long hello_read() {
		return *(long *)0xF0000000;
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
	constexpr uint64_t TRAP_PAGE = 0xF0000000;
	bool trapped_write = false;
	bool trapped_read  = false;

	auto& trap_page =
		machine.memory.create_writable_pageno(Memory<RISCV64>::page_number(TRAP_PAGE));
	trap_page.set_trap(
		[&] (auto&, uint32_t /*offset*/, int mode, int64_t value) {
			switch (Page::trap_mode(mode))
			{
			case TRAP_WRITE:
				trapped_write = true;
				break;
			case TRAP_READ:
				trapped_read = true;
				break;
			}
		});

	// Run for at most X instructions before giving up
	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(machine.return_value<int>() == 666);
	REQUIRE(trapped_read  == false);
	REQUIRE(trapped_write == false);

	machine.vmcall("hello_write");
	REQUIRE(trapped_write == true);
	REQUIRE(trapped_read  == false);

	machine.vmcall("hello_read");
	REQUIRE(trapped_write == true);
	REQUIRE(trapped_read  == true);
}
