#include <string>
#include <unistd.h>
#include <libriscv/machine.hpp>
static inline std::vector<uint8_t> load_file(const std::string&);

static constexpr bool linux_guest = true;
static constexpr bool newlib_mini_guest = false;
#include "linux.hpp"
#include "syscalls.hpp"

int main(int argc, const char** argv)
{
	assert(argc > 1 && "Provide binary filename!");
	const std::string filename = argv[1];

	const auto binary = load_file(filename);

	std::vector<std::string> args = {
		"hello_world", "test!"
	};

	riscv::verbose_machine = false;
	riscv::Machine<riscv::RISCV32> machine { binary };
	machine.install_syscall_handler(riscv::EBREAK_SYSCALL, syscall_ebreak<riscv::RISCV32>);
	machine.install_syscall_handler(64, syscall_write<riscv::RISCV32>);
	machine.install_syscall_handler(93, syscall_exit<riscv::RISCV32>);
	// enough pages for startup + 1mb buffer :)
	machine.memory.set_pages_total(300);

	if constexpr (linux_guest)
	{
		std::vector<std::string> env = {
			"LC_CTYPE=C", "LC_ALL=C", "USER=groot"
		};
		prepare_linux<riscv::RISCV32>(machine, args, env);
		// some extra syscalls
		add_linux_syscalls(machine);
	}
	else if constexpr (newlib_mini_guest)
	{
		// the minimum number of syscalls needed for malloc and C++ exceptions
		add_newlib_syscalls(machine);
		machine.setup_argv(args);
	}

	/*
	machine.verbose_instructions = true;
	machine.cpu.breakpoint(0x39072);
	machine.break_now();
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
	printf("Instructions executed: %zu\n", (size_t) machine.cpu.registers().counter);

	// VM function call testing
	if (machine.address_of("test") != 0)
	{
		printf("\n");
		// make sure stack is aligned for a function call
		machine.realign_stack();
		// reset instruction counter to simplify calculation
		machine.cpu.registers().counter = 0;
		// make a function call into the guest VM
		int ret = machine.vmcall("test", {555}, 3000);
		printf("test returned %d\n", ret);
		printf("Instructions executed: %zu\n", (size_t) machine.cpu.registers().counter);
		// resume execution:
		machine.simulate();
		printf("Instructions executed: %zu\n", (size_t) machine.cpu.registers().counter);
		// extract real return value:
		ret = machine.sysarg<long>(0);
		printf("test *actually* returned %d\n", ret);
	}
	return 0;
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
