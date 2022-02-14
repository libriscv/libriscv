#include <libriscv/machine.hpp>
#include <libriscv/rsp_server.hpp>
#include <include/syscall_helpers.hpp>
#include "settings.hpp"
static inline std::vector<uint8_t> load_file(const std::string&);

static constexpr uint64_t MAX_MEMORY = 1024 * 1024 * 200;

template <int W>
static void run_sighandler(riscv::Machine<W>&);

template <int W>
static void run_program(const std::vector<uint8_t>& binary, const std::string& filename)
{
	const std::vector<std::string> args = {
		filename,
		"test!"
	};

	riscv::Machine<W> machine { binary, {
		.memory_max = MAX_MEMORY
	}};

	if constexpr (full_linux_guest)
	{
		std::vector<std::string> env = {
			"LC_CTYPE=C", "LC_ALL=C", "USER=groot"
		};
		machine.setup_linux(args, env);
		// some extra syscalls
		setup_linux_syscalls(machine);
		// multi-threading
		machine.setup_posix_threads();
	}
	else if constexpr (newlib_mini_guest)
	{
		// the minimum number of syscalls needed for malloc and C++ exceptions
		setup_newlib_syscalls(machine);
		machine.setup_argv(args);
	}
	else if constexpr (micro_guest) {
		machine.setup_argv(args);
		machine.setup_native_heap(5, 0x40000000, 6*1024*1024);
		machine.setup_native_memory(10, false);
		machine.setup_native_threads(30);
		setup_minimal_syscalls(machine);
	}
	else {
		fprintf(stderr, "Unknown emulation mode! Exiting...\n");
		exit(1);
	}

	machine.on_unhandled_syscall = [] (auto&, int number) {
		printf("Unhandled system call: %d\n", number);
	};

	/*
	machine.cpu.breakpoint(machine.address_of("main"));
	machine.cpu.breakpoint(0x10730);
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
#ifdef RISCV_DEBUG
	// print all instructions by default, when debugging is enabled
	machine.verbose_instructions = true;
	machine.print_and_pause();
#endif

	try {
		// If you run the emulator with DEBUG=1, you can
		// connect with GDB built for RISC-V.
		if (getenv("DEBUG")) {
			printf("GDB server is listening on localhost:2159\n");
			riscv::RSP<W> server { machine, 2159 };
			auto client = server.accept();
			if (client != nullptr) {
				printf("GDB is connected\n");
				//client->set_verbose(true);
				while (client->process_one());
			}
			if (!machine.stopped()) {
				// Run remainder of program
				machine.simulate();
			}
		} else {
			// Normal RISC-V simulation
			machine.simulate();
		}
	} catch (riscv::MachineException& me) {
		printf(">>> Machine exception %d: %s (data: 0x%lX)\n",
				me.type(), me.what(), me.data());
#ifdef RISCV_DEBUG
		machine.print_and_pause();
#else
		run_sighandler(machine);
#endif
	} catch (std::exception& e) {
		printf(">>> Exception: %s\n", e.what());
#ifdef RISCV_DEBUG
		machine.print_and_pause();
#else
		run_sighandler(machine);
#endif
	}
	printf(">>> Program exited, exit code = %d\n",
		machine.template return_value<int> ());
	printf("Instructions executed: %zu\n",
		(size_t) machine.instruction_counter());
	printf("Pages in use: %zu (%zu kB memory)\n",
		machine.memory.pages_active(), machine.memory.pages_active() * 4);
}

int main(int argc, const char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "Provide RISC-V binary as argument!\n");
		exit(1);
	}
	const std::string filename = argv[1];

	const auto binary = load_file(filename);
	assert(binary.size() >= 64);

	if (binary[4] == ELFCLASS64)
		run_program<riscv::RISCV64> (binary, filename);
	else
		run_program<riscv::RISCV32> (binary, filename);

	return 0;
}

template <int W>
void run_sighandler(riscv::Machine<W>& machine)
{
	const auto handler = machine.sighandler();
	if (handler == 0x0)
		return;
	machine.set_sighandler(0x0);

	machine.stack_push(machine.cpu.reg(riscv::REG_RA));
	machine.cpu.reg(riscv::REG_RA) = machine.cpu.pc();
	machine.cpu.reg(riscv::REG_ARG0) = 11; /* SIGSEGV */
	machine.cpu.jump(handler);
	machine.simulate(60'000);

	machine.set_sighandler(handler);
}

#include <stdexcept>
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
