#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libriscv/machine.hpp>
#include <libriscv/rv32i_instr.hpp>
extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2 -static", bool cpp = false);
static const uint64_t MAX_MEMORY = 8ul << 20; /* 8MB */
static const uint64_t MAX_INSTRUCTIONS = 10'000'000ul;
using namespace riscv;

/** The new custom instruction **/
static const Instruction<RISCV64> custom {
	[] (auto& cpu, rv32i_instruction instr) {
		printf("Hello custom instruction World!\n");
		REQUIRE(instr.opcode() == 0b1010111);
		cpu.reg(riscv::REG_ARG0) = 0xDEADB33F;
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) {
		return snprintf(buffer, len, "CUSTOM: 4-byte 0x%X (0x%X)",
						instr.opcode(), instr.whole);
	}
};

TEST_CASE("Custom instruction", "[Custom]")
{
	const auto binary = build_and_load(R"M(
int main()
{
	__asm__(".word 0b1010111");
	__asm__("ret");
}
)M");

	CPU<RISCV64>::on_unimplemented_instruction =
	[] (rv32i_instruction instr) -> const Instruction<RISCV64>& {
		if (instr.opcode() == 0b1010111) {
			return custom;
		}
		return CPU<RISCV64>::get_unimplemented_instruction();
	};

	// Normal (fastest) simulation
	{
		riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
		// We need to install Linux system calls for maximum gucciness
		machine.setup_linux_syscalls();
		// We need to create a Linux environment for runtimes to work well
		machine.setup_linux(
			{"va_exec"},
			{"LC_TYPE=C", "LC_ALL=C", "USER=root"});
		// Run for at most X instructions before giving up
		machine.simulate(MAX_INSTRUCTIONS);

		REQUIRE(machine.return_value() == 0xDEADB33F);
	}
	// Precise (step-by-step) simulation
	{
		riscv::Machine<RISCV64> machine{binary, { .memory_max = MAX_MEMORY }};
		machine.setup_linux_syscalls();
		machine.setup_linux(
			{"va_exec"},
			{"LC_TYPE=C", "LC_ALL=C", "USER=root"});
		// Verify step-by-step simulation
		machine.cpu.simulate_precise(MAX_INSTRUCTIONS);

		REQUIRE(machine.return_value() == 0xDEADB33F);
	}
}
