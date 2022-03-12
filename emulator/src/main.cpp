#include <libriscv/machine.hpp>
#include <libriscv/rsp_server.hpp>
#include "settings.hpp"
static inline std::vector<uint8_t> load_file(const std::string&);

static constexpr uint64_t MAX_MEMORY = 1024 * 1024 * 200;

template <int W>
static void run_sighandler(riscv::Machine<W>&);

template <int W>
static void run_program(
	const std::vector<uint8_t>& binary,
	const std::vector<std::string>& args)
{
	riscv::Machine<W> machine { binary, {
		.memory_max = MAX_MEMORY
	}};

	if constexpr (full_linux_guest)
	{
		std::vector<std::string> env = {
			"LC_CTYPE=C", "LC_ALL=C", "USER=groot"
		};
		machine.setup_linux(args, env);
		// Linux system to open files and access internet
		machine.setup_linux_syscalls();
		machine.fds().permit_filesystem = true;
		machine.fds().permit_sockets = true;
		// Only allow opening certain file paths. The void* argument is
		// the user-provided pointer set in the RISC-V machine.
		machine.fds().filter_open = [] (void* user, const char* path) {
			(void) user;
			if (strcmp(path, "/etc/hostname") == 0)
				return true;
			return false;
		};
		// multi-threading
		machine.setup_posix_threads();
	}
	else if constexpr (newlib_mini_guest)
	{
		// the minimum number of syscalls needed for malloc and C++ exceptions
		machine.setup_newlib_syscalls();
		machine.setup_argv(args);
	}
	else if constexpr (micro_guest) {
		machine.setup_argv(args);
		machine.setup_native_heap(1, 0x40000000, 6*1024*1024);
		machine.setup_native_memory(6);
		machine.setup_native_threads(30);
		machine.setup_minimal_syscalls();
	}
	else {
		fprintf(stderr, "Unknown emulation mode! Exiting...\n");
		exit(1);
	}

	/*
	machine.memory.trap(0x3FFFD000,
		[&machine] (riscv::Page& page, uint32_t off, int mode, int64_t val)
		{
			if (mode & riscv::TRAP_WRITE) {
				printf("> write: 0x%X -> 0x%X (%u)\n", off, (int) val, (int) val);
			} else {
				printf("> read: 0x%X -> %d\n", off, page.page().aligned_read<uint32_t> (off));
			}
			machine.print_and_pause();
		});
	machine.cpu.breakpoint(machine.address_of("main"));
	machine.cpu.breakpoint(0x10730);
	machine.cpu.breakpoint(0x5B540, //0x5B518,
		[] (auto& cpu)
		{
			printf("Exchanging SR1 = %u with SR1 = 15\n", cpu.reg(9));
			cpu.reg(9) = 15;
			cpu.machine().print_and_pause();
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
		// If you run the emulator with DEBUG=1, you can connect
		// with gdb-multiarch using target remote localhost:2159.
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
		if (me.type() == riscv::UNIMPLEMENTED_INSTRUCTION || me.type() == riscv::MISALIGNED_INSTRUCTION) {
			printf(">>> Is an instruction extension disabled?\n");
			printf(">>> A-extension: %d  C-extension: %d  F-extension: %d\n",
				riscv::atomics_enabled, riscv::compressed_enabled, riscv::floating_point_enabled);
		}
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
	const auto retval = machine.return_value();
	printf(">>> Program exited, exit code = %ld (0x%lX)\n",
		(long)retval, (long)retval);
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

	std::vector<std::string> args;
	for (int i = 1; i < argc; i++) {
		args.push_back(argv[i]);
	}
	const std::string& filename = args.front();

	const auto binary = load_file(filename);
	assert(binary.size() >= 64);

	try {
		if (binary[4] == ELFCLASS64)
			run_program<riscv::RISCV64> (binary, args);
		else
			run_program<riscv::RISCV32> (binary, args);
	} catch (const std::exception& e) {
		printf("Exception: %s\n", e.what());
	}

	return 0;
}

template <int W>
void run_sighandler(riscv::Machine<W>& machine)
{
	constexpr int SIGSEGV = 11;
	auto& action = machine.sigaction(SIGSEGV);
	auto handler = action.handler;
	if (handler == 0x0 || handler == (riscv::address_type<W>)-1)
		return;
	action.handler = 0x0; // Avoid re-triggering(?)

	machine.stack_push(machine.cpu.reg(riscv::REG_RA));
	machine.cpu.reg(riscv::REG_RA) = machine.cpu.pc();
	machine.cpu.reg(riscv::REG_ARG0) = 11; /* SIGSEGV */
	try {
		machine.cpu.jump(handler);
		machine.simulate(60'000);
	} catch (...) {}

	action.handler = handler;
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
