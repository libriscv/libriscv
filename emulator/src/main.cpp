#include <string>
#include <unistd.h>
#include <libriscv/machine.hpp>
static inline std::vector<uint8_t> load_file(const std::string&);

static constexpr uint64_t MAX_MEMORY = 1024 * 1024 * 24;
static constexpr bool full_linux_guest = false;
static constexpr bool newlib_mini_guest = true;
#include "linux.hpp"
#include "syscalls.hpp"
static void test_vmcall(riscv::Machine<riscv::RISCV32>&, State<riscv::RISCV32>&);
#include "threads.hpp"

int main(int argc, const char** argv)
{
	assert(argc > 1 && "Provide RISC-V binary argument!");
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
		setup_native_heap_syscalls(state, machine);
	}

	/*
	machine.cpu.breakpoint(machine.address_of("main"));
	machine.cpu.breakpoint(0x5B4F4);
	machine.cpu.breakpoint(0x5B540, //0x5B518,
		[] (auto& cpu)
		{
			printf("Exchanging SR1 = %u with SR1 = 15\n", cpu.reg(9));
			cpu.reg(9) = 15;
			cpu.machine().print_and_pause();
		});

	machine.memory.trap(0x3FFFD000,
		[&machine] (riscv::Page& page, uint32_t off, int mode, int64_t val) -> int64_t
		{
			if (off == 0xC3C) {
				if (mode & riscv::TRAP_WRITE) {
					printf("> write: 0x%X -> 0x%X (%u)\n", off, (int) val, (int) val);
				} else {
					printf("> read: 0x%X -> %d\n", off, page.aligned_read<uint32_t> (off));
				}
				machine.print_and_pause();
			}
			return page.passthrough(off, mode, val);
		});


	machine.memory.trap(0x3FFFE000,
		[&machine] (riscv::Page& page, uint32_t off, int mode, int64_t val) -> int64_t
		{
			if (mode & riscv::TRAP_WRITE) {
				printf("> 0x3fffe write: 0x%X -> 0x%X (%c)\n", off, (int) val, (char) val);
			}
			//machine.print_and_pause();
			//machine.verbose_registers = true;
			machine.verbose_instructions = true;
			return page.passthrough(off, mode, val);
		});
	machine.verbose_instructions = true;
	machine.verbose_jumps = true;
	machine.verbose_registers = true;
	machine.verbose_fp_registers = true;
	machine.throw_on_unhandled_syscall = true;
	*/

	try {
		machine.simulate();
	} catch (riscv::MachineException& me) {
		printf(">>> Machine exception %d: %s (data: %d)\n",
				me.type(), me.what(), me.data());
#ifdef RISCV_DEBUG
		machine.print_and_pause();
#endif
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
	test_vmcall(machine, state);
	test_vmcall(machine, state);
	return 0;
}

void test_vmcall(riscv::Machine<riscv::RISCV32>& machine, State<riscv::RISCV32>& state)
{
	// look for a symbol called "test" in the binary
	if (machine.address_of("test") != 0)
	{
		printf("\n");
		// make sure stack is aligned for a function call
		machine.realign_stack();
		// reset instruction counter to simplify calculation
		machine.cpu.registers().counter = 0;
#ifndef RISCV_DEBUG
		state.output.clear();
#endif
		// make a function call into the guest VM, but don't start execution
		machine.vmcall("test", {555}, {}, false);
		do {
			// resume execution, to complete the function call:
			machine.simulate(1000);
			printf("Instruction count: %zu\n", (size_t) machine.cpu.registers().counter);
		} while (!machine.stopped());
		// extract real return value:
		int ret = machine.sysarg<int>(0);
		printf("test *actually* returned %d\n", ret);
#ifndef RISCV_DEBUG
		printf("\n*** Guest output ***\n%s\n", state.output.c_str());
#endif
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
