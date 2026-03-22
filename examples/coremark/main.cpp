#include <fstream>
#include <iostream>
#include <libriscv/machine.hpp>
using namespace riscv;
static const std::string filename = "coremark-rv32g_b.elf";

int main(int argc, char** argv)
{

	// Read the RISC-V program into a std::vector:
	std::ifstream stream(filename, std::ios::in | std::ios::binary);
	if (!stream) {
		std::cout << filename << ": File not found?" << std::endl;
		return -1;
	}
	const std::vector<uint8_t> binary(
		(std::istreambuf_iterator<char>(stream)),
		std::istreambuf_iterator<char>()
	);

	// Take program arguments and make a new string vector, from 1..N
	std::vector<std::string> arguments { "coremark" };

	// Create a new 32-bit RISC-V machine
	Machine<RISCV32> machine{binary, {.memory_max = 256UL << 20}};

	// Minimal Linux/Newlib env and syscall setup
	machine.setup_linux(
		arguments,
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});
	machine.setup_newlib_syscalls();

	try {
		machine.simulate(128'000'000'000ull);
	} catch (const std::exception& e) {
		std::cout << "Program error: " << e.what() << std::endl;
		return -1;
	}

	std::cout << "Program exited with status: " << machine.return_value<long>() << std::endl;
}
