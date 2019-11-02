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

	riscv::Machine<riscv::RISCV32> machine { binary };
	machine.install_syscall_handler(riscv::EBREAK_SYSCALL, syscall_ebreak<riscv::RISCV32>);
	machine.install_syscall_handler(64, syscall_write<riscv::RISCV32>);
	machine.install_syscall_handler(93, syscall_exit<riscv::RISCV32>);
	// enough pages for startup + 1mb buffer :)
	machine.memory.set_pages_total(300);

	if constexpr (linux_guest)
	{
		std::vector<std::string> args = {
			"hello_world", "test!"
		};
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
	}

	/*
	machine.verbose_instructions = true;
	machine.verbose_jumps = true;
	machine.verbose_registers = true;
	machine.break_now();
	machine.cpu.breakpoint(0x17c64);
	*/

	try {
		while (!machine.stopped()) {
			machine.simulate();
		}
	} catch (std::exception& e) {
		printf(">>> Exception: %s\n", e.what());
		//machine.print_and_pause();
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
