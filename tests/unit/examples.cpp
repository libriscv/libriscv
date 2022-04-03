#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libriscv/machine.hpp>
extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2 -static", bool cpp = false);
using namespace riscv;

TEST_CASE("Main example", "[Examples]")
{
	const auto binary = build_and_load(R"M(
	extern void exit(int);
	int main() {
		exit(666);
		return 123;
	})M");

	Machine<RISCV64> machine { binary };
	machine.setup_linux(
		{"myprogram", "1st argument!", "2nd argument!"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});
	machine.setup_linux_syscalls();

	struct State {
		long code = -1;
	} state;
	machine.set_userdata(&state);

	// exit and exit_group
	Machine<RISCV64>::install_syscall_handler(93,
		[] (Machine<RISCV64>& machine) {
			auto* state = machine.get_userdata<State> ();
			state->code = machine.sysarg(0);
			machine.stop();
		});
	Machine<RISCV64>::install_syscall_handler(94,
		Machine<RISCV64>::syscall_handlers.at(93));

	machine.simulate(1'000'000UL);

	REQUIRE(state.code == 666);
	REQUIRE(machine.return_value() == 666);
}
