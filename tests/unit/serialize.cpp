#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libriscv/machine.hpp>
extern std::vector<uint8_t> build_and_load(const std::string& code,
		   const std::string& args = "-O2 -static", bool cpp = false);
static const uint64_t MAX_MEMORY = 8ul << 20; /* 8MB */
static const uint64_t MAX_INSTRUCTIONS = 10'000'000ul;
using namespace riscv;

TEST_CASE("Catch output from write system call", "[Output]")
{
	struct State {
		std::vector<uint8_t> data;
	} state;
	const auto binary = build_and_load(R"M(
	extern long write(int, const void*, unsigned long);
	int main(int argc, char** argv) {
		write(1, argv[0], 12);
		return 666;
	})M");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	// We need to install Linux system calls for maximum gucciness
	machine.setup_linux_syscalls();
	// We need to create a Linux environment for runtimes to work well
	machine.setup_linux(
		{"serialize_me"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	machine.set_userdata(&state);
	machine.set_printer(
		[] (auto& m, const char*, size_t) {
			auto* state = m.template get_userdata<State> ();
			m.serialize_to(state->data);
		});
	// Run for at most X instructions before giving up
	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(machine.return_value<int>() == 666);

	// Restoring the machine using the same binary ensures that we
	// can use fastsim and instruction caching. We would have to disable
	// these features to serialize/restore a machine using only the data.
	riscv::Machine<RISCV64> restored_machine { binary, { .memory_max = MAX_MEMORY } };
	restored_machine.deserialize_from(state.data);

	// Verify some known registers
	REQUIRE(restored_machine.sysarg(0) == 1); // STDOUT_FILENO
	REQUIRE(restored_machine.memory.memstring(restored_machine.sysarg(1)) == "serialize_me");
	REQUIRE(restored_machine.sysarg(2) == 12u);
	REQUIRE(restored_machine.return_value<int>() != 666);

	// Resume the program
	restored_machine.cpu.simulate(MAX_INSTRUCTIONS);

	REQUIRE(restored_machine.return_value<int>() == 666);
}
