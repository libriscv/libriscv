#include <cassert>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <vector>
static inline std::vector<uint8_t> load_file(const std::string&);

#include <libriscv/machine.hpp>
static constexpr bool verbose_syscalls = false;
static constexpr bool verbose_machine = false;

template <int W>
uint32_t syscall_write(riscv::Machine<4>& machine)
{
	const int    fd  = machine.cpu.reg(riscv::RISCV::REG_ARG0);
	const auto   address = machine.cpu.reg(riscv::RISCV::REG_ARG1);
	const size_t len = machine.cpu.reg(riscv::RISCV::REG_ARG2);
	if constexpr (verbose_syscalls) {
		printf("SYSCALL write called, addr = %#X  len = %zu\n", address, len);
	}
	// we only accept standard pipes, for now :)
	if (fd >= 0 && fd < 3) {
		uint8_t buffer[len];
		machine.memory.memcpy_out(buffer, address, len);
		return write(fd, buffer, len);
	}
	return -1;
}
template <int W>
uint32_t syscall_exit(riscv::Machine<W>& machine)
{
	printf(">>> Program exited, exit code = %d\n", machine.cpu.reg(riscv::RISCV::REG_ARG0));
	machine.stop();
	return 0;
}
template <int W>
uint32_t syscall_ebreak(riscv::Machine<W>& machine)
{
	printf("\n>>> EBREAK at %#X", machine.cpu.pc());
	machine.break_now();
	return 0;
}
template <int W>
uint32_t syscall_check(riscv::Machine<W>& machine)
{
	const auto value = machine.cpu.reg(riscv::RISCV::REG_ARG0);
	if constexpr (verbose_syscalls) {
		printf("SYSCALL check called, value = %d (%s)\n", value, (value >= 0 ? "OK" : "ERROR"));
	}
	if (value < 0) {
		printf(">>> Got negative check value: %d\n", value);
		machine.stop();
	}
	return value;
}

int main(int argc, const char** argv)
{
	assert(argc > 1 && "Provide binary filename!");
	const std::string filename = argv[1];

	const auto binary = load_file(filename);

	riscv::Machine<riscv::RISCV32> machine { binary, verbose_machine };
	machine.install_syscall_handler(0, syscall_ebreak<riscv::RISCV32>);
	machine.install_syscall_handler(64, syscall_write<riscv::RISCV32>);
	machine.install_syscall_handler(93, syscall_exit<riscv::RISCV32>);

	/*
	machine.cpu.breakpoint(0x10190);
	machine.verbose_instructions = true;
	machine.verbose_jumps = true;
	machine.verbose_registers = true;
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
