#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libriscv/machine.hpp>
extern std::vector<uint8_t> build_and_load(const std::string& code,
		   const std::string& args = "-O2 -static", bool cpp = false);
static const uint64_t MAX_MEMORY = 8ul << 20; /* 8MB */
static const uint64_t MAX_INSTRUCTIONS = 10'000'000ul;
static const std::vector<uint8_t> empty;
using namespace riscv;

TEST_CASE("Catch output from write system call", "[Serialize]")
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

	riscv::Machine<RISCV64> machine { binary, {
		.memory_max = MAX_MEMORY,
		.use_memory_arena = false
	}};
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

	riscv::Machine<RISCV64> restored_machine { binary, { .memory_max = MAX_MEMORY } };
	restored_machine.deserialize_from(state.data);

	// Verify some known registers
	REQUIRE(restored_machine.sysarg(0) == 1); // STDOUT_FILENO
	REQUIRE(restored_machine.memory.memstring(restored_machine.sysarg(1)) == "serialize_me");
	REQUIRE(restored_machine.sysarg(2) == 12u);
	REQUIRE(restored_machine.return_value<int>() != 666);

	// Resume the program
	restored_machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(restored_machine.return_value<int>() == 666);
}

static std::vector<uint8_t> serialized_from_another_place;

TEST_CASE("Serialized state goes out of scope", "[Serialize]")
{
	const auto binary = build_and_load(R"M(
	extern long write(int, const void*, unsigned long);
	int main(int argc, char** argv) {
		write(1, argv[0], 12);
		return 666;
	})M");

	riscv::Machine<RISCV64> machine { binary, {
		.memory_max = MAX_MEMORY,
		.use_memory_arena = false
	}};
	// We need to install Linux system calls for maximum gucciness
	machine.setup_linux_syscalls();
	// We need to create a Linux environment for runtimes to work well
	machine.setup_linux(
		{"serialize_me"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	// 1. We are creating a completely empty machine, with no binary
	// 2. We are going to let the state go out of scope
	riscv::Machine<RISCV64> restored_machine { empty, { .memory_max = MAX_MEMORY, .use_memory_arena = false } };
	{
		struct State {
			std::vector<uint8_t> data;
		} state;

		machine.set_userdata(&state);
		machine.set_printer(
			[] (auto& m, const char*, size_t) {
				auto* state = m.template get_userdata<State> ();
				m.serialize_to(state->data);
			});
		// Let the original machine finish
		machine.simulate(MAX_INSTRUCTIONS);
		REQUIRE(machine.return_value<int>() == 666);

		// Used by the next unit test
		serialized_from_another_place = state.data;

		// Deserialize from state into restored machine
		REQUIRE(!state.data.empty());
		restored_machine.deserialize_from(state.data);
		// Let the state go out of scope
	}

	// Verify some known registers
	REQUIRE(restored_machine.sysarg(0) == 1); // STDOUT_FILENO
	REQUIRE(restored_machine.memory.memstring(restored_machine.sysarg(1)) == "serialize_me");
	REQUIRE(restored_machine.sysarg(2) == 12u);
	REQUIRE(restored_machine.return_value<int>() != 666);

	// Resume the program
	restored_machine.simulate(MAX_INSTRUCTIONS);
	REQUIRE(restored_machine.return_value<int>() == 666);
}

TEST_CASE("Serialized state from another place", "[Serialize]")
{
	// 1. We are creating a completely empty machine, with no binary
	// 2. We are going to deserialize state from another test and verify it
	riscv::Machine<RISCV64> restored_machine { empty, { .memory_max = MAX_MEMORY, .use_memory_arena = false } };

	restored_machine.deserialize_from(serialized_from_another_place);

	// Verify some known registers
	REQUIRE(restored_machine.sysarg(0) == 1); // STDOUT_FILENO
	REQUIRE(restored_machine.memory.memstring(restored_machine.sysarg(1)) == "serialize_me");
	REQUIRE(restored_machine.sysarg(2) == 12u);
	REQUIRE(restored_machine.return_value<int>() != 666);

	// Resume the program
	restored_machine.simulate(MAX_INSTRUCTIONS);
	REQUIRE(restored_machine.return_value<int>() == 666);
}
