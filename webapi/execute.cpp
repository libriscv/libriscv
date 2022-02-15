#include "server.hpp"

#include <libriscv/machine.hpp>
#include <include/syscall_helpers.hpp>
#include <libriscv/threads.hpp>
using namespace httplib;

// Avoid endless loops, code that takes too long and excessive memory usage
static const uint64_t MAX_BINARY       = 32'000'000UL;
static const uint64_t MAX_INSTRUCTIONS = 6'000'000UL;
static const uint64_t MAX_MEMORY       = 32UL * 1024 * 1024;

static const std::vector<std::string> env = {
	"LC_CTYPE=C", "LC_ALL=C", "USER=groot"
};

extern uint64_t micros_now();
extern uint64_t monotonic_micros_now();

static void
protected_execute(const Request& req, Response& res, const ContentReader& creader)
{
	std::vector<uint8_t> binary;
	creader([&] (const char* data, size_t data_length) {
		if (binary.size() + data_length > MAX_BINARY) return false;
		binary.insert(binary.end(), data, data + data_length);
		return true;
	});
	if (binary.empty()) {
		res.status = 400;
		res.set_header("X-Error", "Empty binary");
	}

	// go-time: create machine, execute code
	riscv::Machine<riscv::RISCV64> machine { binary, {
		.memory_max = MAX_MEMORY
	}};

	machine.setup_linux({"program"}, env);
	setup_linux_syscalls(machine);
	machine.setup_posix_threads();

	std::string output = "";
	machine.set_printer([&output] (const char* text, size_t len) {
		output.append(text, len);
	});

	struct StartupState {
		bool break_used = false;
	} startup_state;
	machine.set_userdata(&startup_state);

	// Stop (pause) the machine when he hit a trap/break instruction
	machine.install_syscall_handler(riscv::SYSCALL_EBREAK,
	[] (auto& machine) {
		auto* state = machine.template get_userdata<StartupState>();
		state->break_used = true;
		machine.stop();
		// When we return from this function the internal PC will increment
		// past this instruction, which allows the Machine to be resumed
		// just after the break, afterwards.
	});

	// Execute until we have hit a break
	const uint64_t st0 = micros_now();
	asm("" : : : "memory");
	try {
		machine.simulate(MAX_INSTRUCTIONS);
	} catch (std::exception& e) {
		res.set_header("X-Exception", e.what());
	}
	asm("" : : : "memory");
	const uint64_t st1 = micros_now();
	asm("" : : : "memory");
	res.set_header("X-Startup-Time", std::to_string(st1 - st0));
	const auto ic = machine.instruction_counter();
	res.set_header("X-Startup-Instructions", std::to_string(ic));
	// cache for 10 seconds (it's only the output of a program)
	res.set_header("Cache-Control", "max-age=10");

	if (startup_state.break_used == true)
	{
		startup_state.break_used = false;
		// Reset PC here for benchmarking
		machine.reset_instruction_counter();
		std::deque<uint64_t> samples;

		asm("" : : : "memory");
		const uint64_t t0 = micros_now();
		asm("" : : : "memory");

		try {
			machine.simulate(MAX_INSTRUCTIONS);
		} catch (const std::exception& e) {
			printf("Exception after break: %s\n", e.what());
			res.set_header("X-Exception", e.what());
		}

		asm("" : : : "memory");
		const uint64_t t1 = micros_now();
		asm("" : : : "memory");
		samples.push_back(t1 - t0);

		if (!samples.empty()) {
			const uint64_t first = samples[0];
			std::sort(samples.begin(), samples.end());
			const uint64_t lowest = samples[0];
			const uint64_t median = samples[samples.size() / 2];
			const uint64_t highest = samples[samples.size()-1];
			res.set_header("X-Runtime-First", std::to_string(first));
			res.set_header("X-Runtime-Lowest", std::to_string(lowest));
			res.set_header("X-Runtime-Median", std::to_string(median));
			res.set_header("X-Runtime-Highest", std::to_string(highest));
		}
		const auto ic = machine.instruction_counter();
		res.set_header("X-Instruction-Count", std::to_string(ic));

		// If ebreak was used again to delineate the benchmark,
		// we can finish the execution of main here without
		// resetting the instruction counter.
		if (startup_state.break_used) {
			try {
				machine.simulate(MAX_INSTRUCTIONS);
			} catch (const std::exception& e) {
				printf("Exception after break: %s\n", e.what());
				res.set_header("X-Exception", e.what());
			}
		}
	}
	else {
		res.set_header("X-Instruction-Count", "0");
	}

	res.set_header("X-Binary-Size", std::to_string(binary.size()));
	const size_t active_mem = machine.memory.pages_active() * 4096;
	res.set_header("X-Memory-Usage", std::to_string(active_mem));
	res.set_header("X-Memory-Max", std::to_string(MAX_MEMORY));
	res.set_content(output, "text/plain");

	// A0 is both a return value and first argument, matching
	// any calls to exit()
	const int exit_code = machine.cpu.reg(10);
	res.status = 200;
	res.set_header("X-Exit-Code", std::to_string(exit_code));
}

void execute(const Request& req, Response& res, const ContentReader& creader)
{
	try {
		protected_execute(req, res, creader);
	} catch (std::exception& e) {
		res.status = 200;
		res.set_header("X-Error", e.what());
	}
}
