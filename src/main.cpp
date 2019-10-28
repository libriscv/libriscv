#include <string>
#include <unistd.h>
#include <libriscv/machine.hpp>
static inline std::vector<uint8_t> load_file(const std::string&);

static constexpr bool verbose_syscalls = true;
static constexpr bool verbose_machine  = true;
static constexpr bool linux_guest = true;
#include "linux.hpp"
#include "syscalls.hpp"

int main(int argc, const char** argv)
{
	assert(argc > 1 && "Provide binary filename!");
	const std::string filename = argv[1];

	const auto binary = load_file(filename);

	riscv::Machine<riscv::RISCV32> machine { binary, verbose_machine };
	machine.install_syscall_handler(0, syscall_ebreak<riscv::RISCV32>);
	machine.install_syscall_handler(64, syscall_write<riscv::RISCV32>);
	machine.install_syscall_handler(93, syscall_exit<riscv::RISCV32>);

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
		machine.install_syscall_handler(57, syscall_close<riscv::RISCV32>);
		machine.install_syscall_handler(80, syscall_stat<riscv::RISCV32>);
		machine.install_syscall_handler(214, syscall_brk<riscv::RISCV32>);
	}

	/*
	machine.verbose_instructions = true;
	machine.verbose_jumps = true;
	machine.break_now();
	machine.verbose_registers = true;
	machine.cpu.breakpoint(0x10142);
	*/
	while (!machine.stopped())
	{
		machine.simulate();
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
