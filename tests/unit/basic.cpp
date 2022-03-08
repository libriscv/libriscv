#include <catch2/catch_test_macros.hpp>

#include <libriscv/machine.hpp>
extern std::vector<uint8_t> build_and_load(const std::string& code);
static const uint64_t MAX_MEMORY = 8ul << 20; /* 8MB */
static const uint64_t MAX_INSTRUCTIONS = 10'000'000ul;
using namespace riscv;

TEST_CASE("Instantiate machines", "[Instantiate]")
{
	const auto binary = build_and_load(R"M(
int main() {
	return 666;
})M");
	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };

	// The stack is usually set to under the program area
	// but the RISC-V toolchain is insane and starts at 0x10000.
	REQUIRE(machine.memory.stack_initial() == 0x40000000);
	// The starting address is somewhere in the program area
	REQUIRE(machine.memory.start_address() > 0x10000);
}

TEST_CASE("Catch output from write system call", "[Output]")
{
	bool output_is_hello_world = false;
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

	machine.set_printer([&] (const char* data, size_t size) {
		std::string text{data, data + size};
		output_is_hello_world = (text == "Hello World!");
	});
	// Run for at most 4 seconds before giving up
	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(machine.return_value<int>() == 666);

	// We require that the write system call forwarded to the printer
	// and the data matched 'Hello World!'.
	REQUIRE(output_is_hello_world);
}
