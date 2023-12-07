#include <libriscv/machine.hpp>
#include <libriscv/debug.hpp>
#include <libriscv/rsp_server.hpp>
#include <inttypes.h>
#include <chrono>
#include "settings.hpp"
static inline std::vector<uint8_t> load_file(const std::string&);
static constexpr uint64_t MAX_MEMORY = 1024ULL << 20;

template <int W>
static void run_sighandler(riscv::Machine<W>&);

template <int W>
static void run_program(
	const std::vector<uint8_t>& binary,
	const std::vector<std::string>& args)
{
	const bool debugging_enabled = getenv("DEBUG") != nullptr;

	riscv::Machine<W> machine { binary, {
		.memory_max = MAX_MEMORY,
		.verbose_loader = (getenv("VERBOSE") != nullptr)
	}};

	if constexpr (full_linux_guest)
	{
		std::vector<std::string> env = {
			"LC_CTYPE=C", "LC_ALL=C", "RUST_BACKTRACE=full"
		};
		machine.setup_linux(args, env);
		// Linux system to open files and access internet
		machine.setup_linux_syscalls();
		machine.fds().permit_filesystem = true;
		machine.fds().permit_sockets = true;
		// Rewrite certain links to masquerade and simplify some interactions (eg. /proc/self/exe)
		machine.fds().filter_readlink = [=] (void* user, std::string& path) {
			if (path == "/proc/self/exe") {
				path = "/program";
				return true;
			}
			fprintf(stderr, "Guest wanted to readlink: %s (denied)\n", path.c_str());
			return false;
		};
		// Only allow opening certain file paths. The void* argument is
		// the user-provided pointer set in the RISC-V machine.
		machine.fds().filter_open = [=] (void* user, std::string& path) {
			(void) user;
			if (path == "/etc/hostname"
				|| path == "/etc/hosts"
				|| path == "/etc/nsswitch.conf"
				|| path == "/etc/host.conf"
				|| path == "/etc/resolv.conf")
				return true;
			if (path == "/dev/urandom")
				return true;
			if (path == "/program") { // Fake program path
				path = args.at(0); // Sneakily open the real program instead
				return true;
			}
			if (path == "/etc/ssl/certs/ca-certificates.crt")
				return true;
			// ld-linux
			if (path == "/lib/riscv64-linux-gnu/ld-linux-riscv64-lp64d.so.1") {
				path = "/usr/riscv64-linux-gnu/lib/ld-linux-riscv64-lp64d.so.1";
				return true;
			}
			// libc6
			if (path == "/lib/riscv64-linux-gnu/libc.so.6") {
				path = "/usr/riscv64-linux-gnu/lib/libc.so.6";
				return true;
			}
			// libresolv
			if (path == "/lib/riscv64-linux-gnu/libresolv.so.2") {
				path = "/usr/riscv64-linux-gnu/lib/libresolv.so.2";
				return true;
			}
			// libnss_dns && libnss_files
			if (path == "/lib/riscv64-linux-gnu/libnss_dns.so.2") {
				path = "/usr/riscv64-linux-gnu/lib/libnss_dns.so.2";
				return true;
			}
			if (path == "/lib/riscv64-linux-gnu/libnss_files.so.2") {
				path = "/usr/riscv64-linux-gnu/lib/libnss_files.so.2";
				return true;
			}
			fprintf(stderr, "Guest wanted to open: %s (denied)\n", path.c_str());
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
	else if constexpr (micro_guest)
	{
		// This guest has accelerated libc functions, which
		// are provided as system calls
		// See: tests/unit/native.cpp and tests/unit/include/native_libc.h
		constexpr size_t heap_size = 6ULL << 20; // 6MB
		auto heap = machine.memory.mmap_allocate(heap_size);

		machine.setup_native_heap(470, heap, heap_size);
		machine.setup_native_memory(475);
		machine.setup_native_threads(490);

		machine.setup_newlib_syscalls();
		machine.setup_argv(args);
	}
	else {
		fprintf(stderr, "Unknown emulation mode! Exiting...\n");
		exit(1);
	}

	// A CLI debugger used when DEBUG=1
	riscv::DebugMachine debug { machine };

	if (debugging_enabled)
	{
		// Print all instructions by default
		const bool vi = true;
		// With VERBOSE=1 we also print register values after
		// every instruction.
		const bool vr = (getenv("VERBOSE") != nullptr);
		// If you want to start debugging from the beginning,
		// set FROM_START=1.
		const bool debug_from_start = getenv("FROM_START") != nullptr;

		auto main_address = machine.address_of("main");
		if (debug_from_start || main_address == 0x0) {
			debug.verbose_instructions = vi;
			debug.verbose_registers = vr;
			// Without main() this is a custom or stripped program,
			// so we break immediately.
			debug.print_and_pause();
		} else {
			// Automatic breakpoint at main() to help debug certain programs
			debug.breakpoint(main_address,
			[vi, vr] (auto& debug) {
				auto& cpu = debug.machine.cpu;
				// Remove the breakpoint to speed up debugging
				debug.erase_breakpoint(cpu.pc());
				debug.verbose_instructions = vi;
				debug.verbose_registers = vr;
				printf("\n*\n* Entered main() @ 0x%" PRIX64 "\n*\n", uint64_t(cpu.pc()));
				debug.print_and_pause();
			});
		}
	}

	auto t0 = std::chrono::high_resolution_clock::now();
	try {
		// If you run the emulator with GDB=1, you can connect
		// with gdb-multiarch using target remote localhost:2159.
		if (getenv("GDB")) {
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
		} else if (debugging_enabled) {
			// CLI debug simulation
			debug.simulate();
		} else {
			// Normal RISC-V simulation
			machine.simulate();
		}
	} catch (riscv::MachineException& me) {
		printf("%s\n", machine.cpu.current_instruction_to_string().c_str());
		printf(">>> Machine exception %d: %s (data: 0x%" PRIX64 ")\n",
				me.type(), me.what(), me.data());
		printf("%s\n", machine.cpu.registers().to_string().c_str());
		machine.memory.print_backtrace(
			[] (std::string_view line) {
				printf("-> %.*s\n", (int)line.size(), line.begin());
			});
		if (me.type() == riscv::UNIMPLEMENTED_INSTRUCTION || me.type() == riscv::MISALIGNED_INSTRUCTION) {
			printf(">>> Is an instruction extension disabled?\n");
			printf(">>> A-extension: %d  C-extension: %d  V-extension: %d\n",
				riscv::atomics_enabled, riscv::compressed_enabled, riscv::vector_extension);
		}
		if (debugging_enabled)
			debug.print_and_pause();
		else
			run_sighandler(machine);
	} catch (std::exception& e) {
		printf(">>> Exception: %s\n", e.what());
		machine.memory.print_backtrace(
			[] (std::string_view line) {
				printf("-> %.*s\n", (int)line.size(), line.begin());
			});
		if (debugging_enabled)
			debug.print_and_pause();
		else
			run_sighandler(machine);
	}
	auto t1 = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double> runtime = t1 - t0;

	const auto retval = machine.return_value();
	// You can silence this output by setting SILENT=1, like so:
	// SILENT=1 ./rvlinux myprogram
	if (getenv("SILENT") == nullptr) {
		printf(">>> Program exited, exit code = %" PRId64 " (0x%" PRIX64 ")\n",
			int64_t(retval), uint64_t(retval));
		printf("Instructions executed: %" PRIu64 "  Runtime: %.3fms  Insn/s: %.0fmi/s\n",
			machine.instruction_counter(), runtime.count()*1000.0,
			machine.instruction_counter() / (runtime.count() * 1e6));
		printf("Pages in use: %zu (%zu kB virtual memory, total %zu kB)\n",
			machine.memory.pages_active(),
			machine.memory.pages_active() * 4,
			machine.memory.memory_usage_total() / 1024UL);
	}
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
	constexpr int SIG_SEGV = 11;
	auto& action = machine.sigaction(SIG_SEGV);
	if (action.is_unset())
		return;

	auto handler = action.handler;
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
