#include <string>
#include <unistd.h>
#include <libriscv/machine.hpp>
static inline std::vector<uint8_t> load_file(const std::string&);

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
		machine.install_syscall_handler(56, syscall_openat<riscv::RISCV32>);
		machine.install_syscall_handler(57, syscall_close<riscv::RISCV32>);
		machine.install_syscall_handler(66, syscall_writev<riscv::RISCV32>);
		machine.install_syscall_handler(78, syscall_readlinkat<riscv::RISCV32>);
		machine.install_syscall_handler(80, syscall_stat<riscv::RISCV32>);
		machine.install_syscall_handler(135, syscall_spm<riscv::RISCV32>);
		machine.install_syscall_handler(160, syscall_uname<riscv::RISCV32>);
		machine.install_syscall_handler(174, syscall_getuid<riscv::RISCV32>);
		machine.install_syscall_handler(175, syscall_geteuid<riscv::RISCV32>);
		machine.install_syscall_handler(176, syscall_getgid<riscv::RISCV32>);
		machine.install_syscall_handler(177, syscall_getegid<riscv::RISCV32>);
		machine.install_syscall_handler(214, syscall_brk<riscv::RISCV32>);
		machine.install_syscall_handler(222, syscall_mmap<riscv::RISCV32>);
	}

	machine.verbose_instructions = true;
	/*
	machine.verbose_jumps = true;
	machine.break_now();
	machine.verbose_registers = true;
	*/

	// REQUIRES: 0x3fff100 in main()
	// $6 = (_Unwind_Personality_Fn *) 0x3fff078
	// $7 = (_Unwind_Personality_Fn *) 0x3fff078

	// GOAL: find out how the wrong address gets set for fs.personality in unwind.inc
	// HOW : follow the address backwards?
	//       however, the stack address for the source is not even the same
	//       so, we have to figure out why the source address for fs.personality
	//       is different -> why the stack is different by 32 bytes
	//       1. it should be an instruction failure, missing 32 bytes somehow
	//       2. it could be anywhere, but its related to stack and the
	//          unwinder is possible lazy-loaded, so could be after main()


	machine.memory.trap(0x3FFFEFC8,
		[] (auto& mem, uint32_t addr, int, uint32_t value) -> bool {
			printf("Caught BAD address 0x%X  (value=0x%X)\n", addr, value);
			mem.machine().print_and_pause();
			return true;
		});
	machine.memory.trap(0x3ffea68,
		[] (auto& mem, uint32_t addr, int, uint32_t value) -> bool {
			printf("Caught GOOD address 0x%X  (value=0x%X)\n", addr, value);
			mem.machine().print_and_pause();
			return true;
		});
	machine.cpu.breakpoint(0x1653c);

	try {
		while (!machine.stopped()) {
			machine.simulate();
		}
	} catch (std::exception& e) {
		printf(">>> Exception: %s\n", e.what());
		machine.print_and_pause();
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
