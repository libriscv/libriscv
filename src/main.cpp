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

	// 0x1a546 <ptmalloc_init.part.0>:
	// 0x1c6c6 <tcache_init.part.0>:
	// [0001C6D8] 100427AF LR.W A5 <- [SR0]
	// LR.W on 0x66444
	// [0001C6DC] E781 C.BNEZ A5, PC+8 (0x1C6E4)
	// [0001C6DE] 1CE426AF SC.W A3 <- [SR0], A4
	// SC.W on 0x66444
	// 0x1bc0c <_int_malloc>:
	//    1be06:       22e8f863                bgeu    a7,a4,1c036 <_int_malloc+0x42a>
	// 1c036 is malloc_printerr -> abort

	// 0x10510: return value from _dl_discover_osversion moved into a5
	// 0x10528: the comparison between a4 and a5 determines if version too old
	//machine.cpu.breakpoint(0x1BDFE); // lw a4, [a5+4]
	// A5 is 0, meaning av is NULL??
	//machine.cpu.breakpoint(0x1BE06);
	//machine.cpu.breakpoint(0x1655a);
	// NOTE: second time a6 should be 0x10aa2, but instead its 0x2AEF0

	// REQUIRES: 0x3fff100 in main()
	// $6 = (_Unwind_Personality_Fn *) 0x3fff078
	// $7 = (_Unwind_Personality_Fn *) 0x3fff078

	machine.memory.trap(0x3fff078,
		[] (auto& mem, uint32_t, int, uint32_t) -> bool {
			mem.machine().print_and_pause();
			return true;
		});
	//machine.cpu.breakpoint(0x17578);
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
