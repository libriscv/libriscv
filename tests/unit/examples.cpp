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

#include <libriscv/rv32i_instr.hpp>

TEST_CASE("One instruction at a time", "[Examples]")
{
	const auto binary = build_and_load(R"M(
	extern void exit(int);
	int main() {
		return 0x1234;
	})M");

	Machine<RISCV64> machine{binary};
	machine.setup_linux(
		{"myprogram"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});
	machine.setup_linux_syscalls();

	machine.set_max_instructions(1'000'000UL);

	while (!machine.stopped()) {
		auto& cpu = machine.cpu;
		// Get 32- or 16-bits instruction
		auto instr = cpu.read_next_instruction();
		// Print the instruction to terminal
		printf("%s\n",
			cpu.current_instruction_to_string().c_str());
		// Decode instruction to get instruction info
		auto decoded = cpu.decode(instr);
		// Execute one instruction, and increment PC
		decoded.handler(cpu, instr);
		cpu.increment_pc(instr.length());
	}

	REQUIRE(machine.return_value() == 0x1234);
}

TEST_CASE("Build machine from empty", "[Examples]")
{
	Machine<RISCV32> machine;
	machine.setup_minimal_syscalls();

	const std::vector<uint32_t> my_program {
		0x29a00513, //        li      a0,666
		0x05d00893, //        li      a7,93
		0x00000073, //        ecall
	};

	// Set main execute segment (12 instruction bytes)
	const uint32_t dst = 0x1000;
	machine.cpu.init_execute_area(my_program.data(), dst, 12);

	// Jump to the start instruction
	machine.cpu.jump(dst);

	// Geronimo!
	machine.simulate(1'000ul);

	REQUIRE(machine.return_value() == 666);
}
