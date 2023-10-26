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
	machine.simulate<false>(2);
	REQUIRE(machine.instruction_counter() == 3);
	REQUIRE(machine.cpu.reg(REG_ARG7) == 93);

	machine.cpu.reg(REG_ARG7) = 0;

	machine.simulate<false>(2);
	REQUIRE(machine.instruction_counter() == 5);
	REQUIRE(machine.cpu.reg(REG_ARG7) == 93);
}

TEST_CASE("Crashing payload", "[Micro]")
{
	static constexpr uint32_t MAX_CYCLES = 5'000;

	// Print-out from GDB
	const char elf[] = "\177ELF\002\002\002\216\002\002\002\002\002\002\f\004\002\000\363\000z\003\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\a\000\000\000\000\000\000\000\004\000\000\000\000\000\000\000\000\000\256\377\377\377\373\377\377\000\000`\260\000\217\377P\377\377\377\377\377\377\017\377\377\377\377\377\377\377\377\377\377\377\377\377\377\377\377\377\377\377\377\377\377\002\002\002\002\002\000\374\376\375\377\f\377\377\327\000\000\000\000\000\366\000\000\000\000\000\000\000\000\000\000\000\374~EL\271\372\001\002\213\375\375\375\375\002\002\377\004\002\000\363\000\000\000\000\000\000\000\001\000\000\000\221\377d\000\374\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000~\000\000\000\000\000\000\000\000\374\257\000\000\377\001\000\000\000\000\000\000\020\000\000\000\000\000\000\000\000\370\377\377\b\001\020b\000>>>>>>>>>>>>\000\002\002\002\002\002\002\002\002\002\002\002\002\002\002\002\263\002\002\002\002\002\002\002\000\006\000\005\000\002\002\002\002\002\002\002\002\002\303\303\303\303\303\303\323\303\303\303\303\002\002\023\002\002\263E\000\002\002\002\361\361\361\361\361\361\361\361\361\361\361\361\361\361\361\361\361\361\361\361\002\002\002\002\002\002\002\231\231\231\231\000\000\377\377\377\377\377\377\377\f\377\377\f\370\231LF\002z\002\377\377\000\002\000\363\000\177ELF\200\000\000\000\000\000\000\000\002";
	std::string_view bin { elf, sizeof(elf)-1 };

	const MachineOptions<8> options {
		.allow_write_exec_segment = true,
		.use_memory_arena = false
	};
	try {
		Machine<8> machine { bin, options };
		machine.on_unhandled_syscall = [] (auto&, size_t) {};
		machine.simulate(MAX_CYCLES);
	} catch (const std::exception& e) {
		//printf(">>> Exception: %s\n", e.what());
	}
}
