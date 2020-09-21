#include "server.hpp"

#include <libriscv/machine.hpp>
#include <include/syscall_helpers.hpp>
#include <include/threads.hpp>
using namespace httplib;

// avoid endless loops, code that takes too long and excessive memory usage
static const uint64_t MAX_BINARY       = 16'000'000;
static const uint64_t MAX_INSTRUCTIONS = 2'000'000;
static const uint32_t MAX_MEMORY       = 32 * 1024 * 1024;
static const uint32_t BENCH_SAMPLES    = 100;

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

	State<4> state;
	// go-time: create machine, execute code
	riscv::Machine<riscv::RISCV32> machine { binary, MAX_MEMORY };

	machine.setup_linux({"program"}, env);
	setup_linux_syscalls(state, machine);
	setup_multithreading(state, machine);

	// run the machine until potential break
	bool break_used = false;
	machine.install_syscall_handler(0,
	[&break_used] (auto& machine) -> long {
		break_used = true;
		machine.stop();
		return 0;
	});

	// execute until we are inside main()
	uint32_t main_address = 0x0;
	try {
		main_address = machine.address_of("main");
		if (main_address == 0x0) {
			res.set_header("X-Exception", "The address of main() was not found");
		}
	} catch (std::exception& e) {
		res.set_header("X-Exception", e.what());
	}
	if (main_address != 0x0)
	{
		const uint64_t st0 = micros_now();
		asm("" : : : "memory");
		// execute insruction by instruction until
		// we have entered main(), then break
		try {
			while (LIKELY(!machine.stopped())) {
				machine.cpu.simulate();
				if (UNLIKELY(machine.cpu.instruction_counter() >= MAX_INSTRUCTIONS))
					break;
				if (machine.cpu.registers().pc == main_address)
					break;
			}
		} catch (std::exception& e) {
			res.set_header("X-Exception", e.what());
		}
		asm("" : : : "memory");
		const uint64_t st1 = micros_now();
		asm("" : : : "memory");
		res.set_header("X-Startup-Time", std::to_string(st1 - st0));
		const auto instructions = machine.cpu.instruction_counter();
		res.set_header("X-Startup-Instructions", std::to_string(instructions));
		// cache for 10 seconds (it's only the output of a program)
		res.set_header("Cache-Control", "max-age=10");
	}
	if (machine.cpu.registers().pc == main_address)
	{
		// reset PC here for benchmarking
		machine.cpu.reset_instruction_counter();
		std::deque<uint64_t> samples;
		state.output.clear();

		asm("" : : : "memory");
		const uint64_t t0 = micros_now();
		asm("" : : : "memory");

		try {
			machine.simulate(MAX_INSTRUCTIONS);
			if (machine.cpu.instruction_counter() == MAX_INSTRUCTIONS) {
				res.set_header("X-Exception", "Maximum instructions reached");
			}
		} catch (const std::exception& e) {
			res.set_header("X-Exception", e.what());
		}

		asm("" : : : "memory");
		const uint64_t t1 = micros_now();
		asm("" : : : "memory");
		samples.push_back(t1 - t0);

		res.status = 200;
		res.set_header("X-Exit-Code", std::to_string(state.exit_code));
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
		const auto instructions = std::to_string(machine.cpu.instruction_counter());
		res.set_header("X-Instruction-Count", instructions);
		res.set_header("X-Binary-Size", std::to_string(binary.size()));
		const size_t active_mem = machine.memory.pages_active() * 4096;
		res.set_header("X-Memory-Usage", std::to_string(active_mem));
		res.set_header("X-Memory-Max", std::to_string(MAX_MEMORY));
		res.set_content(state.output, "text/plain");
	}
	else {
		res.set_header("X-Exception", "Could not enter main()");
		res.set_header("X-Instruction-Count", std::to_string(MAX_INSTRUCTIONS));
	}
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
