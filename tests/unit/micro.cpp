#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libriscv/machine.hpp>
#include <libriscv/debug.hpp>
extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2 -static", bool cpp = false);
using namespace riscv;

TEST_CASE("Run exactly X instructions", "[Micro]")
{
	Machine<RISCV32> machine;

	std::array<uint32_t, 3> my_program{
		0x29a00513, //        li      a0,666
		0x05d00893, //        li      a7,93
		0xffdff06f, //        jr      -4
	};

	const uint32_t dst = 0x1000;
	machine.copy_to_guest(dst, &my_program[0], sizeof(my_program));
	machine.memory.set_page_attr(dst, riscv::Page::size(), {
		.read = false,
		.write = false,
		.exec = true
	});
	machine.cpu.jump(dst);

	// Step instruction by instruction using
	// a debugger.
	riscv::DebugMachine debugger{machine};
	debugger.verbose_instructions = true;

	debugger.simulate(3);
	REQUIRE(machine.cpu.reg(REG_ARG0) == 666);
	REQUIRE(machine.cpu.reg(REG_ARG7) == 93);
	REQUIRE(machine.instruction_counter() == 3);

	machine.cpu.reg(REG_ARG7) = 0;

	debugger.simulate(2);
	REQUIRE(machine.instruction_counter() == 5);
	REQUIRE(machine.cpu.reg(REG_ARG7) == 93);

	// Reset CPU registers and counter
	machine.cpu.registers() = {};
	machine.cpu.jump(dst);
	machine.reset_instruction_counter();

	// Normal simulation
	// XXX: Fast compressed simulation will overestimate
	// the instruction counting on purpose, in order to
	// be close in performance compared to uncompressed.
	machine.simulate<false>(2);
	REQUIRE(machine.instruction_counter() >= 3);
	REQUIRE(machine.cpu.reg(REG_ARG7) == 93);

	machine.cpu.reg(REG_ARG7) = 0;

	machine.simulate<false>(2);
	REQUIRE(machine.instruction_counter() >= 5);
	REQUIRE(machine.cpu.reg(REG_ARG7) == 93);
}
