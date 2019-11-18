#include <string>
#include <unistd.h>
#include <libriscv/machine.hpp>
static inline std::vector<uint8_t> load_file(const std::string&);
static void test_vmcall(riscv::Machine<riscv::RISCV32>& machine);

static constexpr uint64_t MAX_MEMORY = 1024 * 1024 * 24;
static constexpr bool full_linux_guest = true;
static constexpr bool newlib_mini_guest = false;
#include "linux.hpp"
#include "syscalls.hpp"
#include "threads.hpp"

int main(int argc, const char** argv)
{
	assert(argc > 1 && "Provide binary filename!");
	const std::string filename = argv[1];

	const auto binary = load_file(filename);

	std::vector<std::string> args = {
		"hello_world", "test!"
	};

	riscv::verbose_machine = false;
	riscv::Machine<riscv::RISCV32> machine { binary, MAX_MEMORY };

	// somewhere to store the guest outputs and exit status
	State<riscv::RISCV32> state;

	if constexpr (full_linux_guest)
	{
		std::vector<std::string> env = {
			"LC_CTYPE=C", "LC_ALL=C", "USER=groot"
		};
		prepare_linux<riscv::RISCV32>(machine, args, env);
		// some extra syscalls
		setup_linux_syscalls(state, machine);
		// multi-threading
		setup_multithreading(state, machine);
	}
	else if constexpr (newlib_mini_guest)
	{
		// the minimum number of syscalls needed for malloc and C++ exceptions
		setup_newlib_syscalls(state, machine);
		machine.setup_argv(args);
	}
	else {
		setup_minimal_syscalls(state, machine);
	}

	/*
	machine.verbose_instructions = true;
	machine.cpu.breakpoint(0x39072);
	machine.verbose_jumps = true;
	machine.verbose_registers = true;
	machine.throw_on_unhandled_syscall = true;
	machine.memory.trap(0x8FFFF000,
		[] (riscv::Page& page, uint32_t off, int mode, int64_t val) -> int64_t
		{
			return page.passthrough(off, mode, val);
		});
	*/

	try {
		machine.simulate();
	} catch (std::exception& e) {
		printf(">>> Exception: %s\n", e.what());
#ifdef RISCV_DEBUG
		machine.print_and_pause();
#endif
	}
	printf(">>> Program exited, exit code = %d\n", state.exit_code);
	printf("Instructions executed: %zu\n", (size_t) machine.cpu.registers().counter);
#ifndef RISCV_DEBUG
	printf("\n*** Guest output ***\n%s\n", state.output.c_str());
#endif
	printf("Pages in use: %zu (%zu kB memory), highest: %zu (%zu kB memory)\n",
			machine.memory.pages_active(), machine.memory.pages_active() * 4,
			machine.memory.pages_highest_active(), machine.memory.pages_highest_active() * 4);

	// VM function call testing
	test_vmcall(machine);
	return 0;
}

void test_vmcall(riscv::Machine<riscv::RISCV32>& machine)
{
	// look for a symbol called "test" in the binary
	if (machine.address_of("test") != 0)
	{
		printf("\n");
		// make sure stack is aligned for a function call
		machine.realign_stack();
		// reset instruction counter to simplify calculation
		machine.cpu.registers().counter = 0;
		// make a function call into the guest VM, stopping at 3000 instructions
		int ret = machine.vmcall("test", {555}, 3000);
		printf("test returned %d\n", ret);
		printf("Instructions executed: %zu\n", (size_t) machine.cpu.registers().counter);
		// resume execution, to complete the function call:
		machine.simulate();
		printf("Instructions executed: %zu\n", (size_t) machine.cpu.registers().counter);
		// extract real return value:
		ret = machine.sysarg<int>(0);
		printf("test *actually* returned %d\n", ret);
	}
}

#include <unistd.h>
std::vector<uint8_t> load_file(const std::string& filename)
{
    size_t size = 0;
    FILE* f = fopen(filename.c_str(), "rb");
    if (f == NULL) throw std::runtime_error("Could not open file: " + filename);

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::vector<uint8_t> result(size);
    if (size != fread(result.data(), 1, size, f))
    {
        fclose(f);
        throw std::runtime_error("Error when reading from file: " + filename);
    }
    fclose(f);
    return result;
}
