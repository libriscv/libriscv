#include <httplib.h>
#include <cstdio>
#include <sys/time.h>
#include <stdexcept>
#include <string>
#include <array>
#include <libriscv/machine.hpp>
#include <syscalls.hpp>
#include <threads.hpp>
#include <linux.hpp>
using namespace std; // string literals

static const char* ADDRESS = "localhost";
static const uint16_t PORT = 1234;
// avoid endless loops, code that takes too long and excessive memory usage
static const uint64_t MAX_INSTRUCTIONS = 2'000'000;
static const uint32_t MAX_MEMORY       = 32 * 1024 * 1024;
static const uint32_t BENCH_SAMPLES    = 100;

static const std::vector<std::string> env = {
	"LC_CTYPE=C", "LC_ALL=C", "USER=groot"
};

static int create_folder(const std::string& folder);
static int python_sanitize_compile(const std::string& pbase, const std::string& pdir, const std::string& met);
static int write_file(const std::string& file, const std::string& text);
static std::vector<uint8_t> load_file(const std::string& filename);
static uint64_t micros_now();

inline void common_response_fields(httplib::Response& res, int status)
{
	res.status = status;
	res.set_header("Access-Control-Allow-Origin", "*");
	res.set_header("Access-Control-Expose-Headers", "*");
}
static std::string project_base() {
	return "/tmp/programs";
}
static std::string project_dir(const int id) {
	return "program" + std::to_string(id);
}
static std::string project_path(const int id) {
	return project_base() + "/program" + std::to_string(id);
}

int main(void)
{
    using namespace httplib;
    Server svr;

    svr.Post("/compile", [](const Request& req, Response& res)
	{
		static size_t request_ID = 0;
		const size_t program_id = request_ID++;
		res.set_header("X-Program-Id", std::to_string(program_id));

		// find compiler method
		std::string method = "linux";
		auto mit = req.params.find("method");
		if (mit != req.params.end()) method = mit->second;
		res.set_header("X-Method", method);

		const std::string progpath = project_path(program_id);
		// create project folder
		if (create_folder(progpath) < 0) {
			if (errno != EEXIST) {
				common_response_fields(res, 200);
				res.set_header("X-Error", "Failed to create project folder");
				return;
			}
		}

		// write code into project folder
		if (write_file(progpath + "/code.cpp", req.body) != 0) {
			common_response_fields(res, 200);
			res.set_header("X-Error", "Failed to write codefile");
			return;
		}

		// sanitize + compile code
		const uint64_t c0 = micros_now();
		const int cc = python_sanitize_compile(project_base(), project_dir(program_id), method);
		if (cc != 0) {
			common_response_fields(res, 200);
			auto vec = load_file(progpath + "/status.txt");
			res.set_header("X-Error", "Compilation failed");
			res.set_content((const char*) vec.data(), vec.size(), "text/plain");
			return;
		}
		const uint64_t c1 = micros_now();
		res.set_header("X-Compile-Time", std::to_string(c1 - c0));
		res.set_header("X-Time-Unit", "10e-6");

		// load binary and execute code
		auto binary = load_file(progpath + "/binary");
		if (binary.empty()) {
			common_response_fields(res, 200);
			res.set_header("X-Error", "Failed to open binary");
			return;
		}

		State<4> state;
		// go-time: create machine, execute code
		riscv::Machine<riscv::RISCV32> machine { binary, MAX_MEMORY };

		prepare_linux<riscv::RISCV32>(machine,
			{"program", std::to_string(program_id)}, env);
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
		auto main_address = machine.address_of("main");
		if (main_address == 0x0) {
			res.set_header("X-Exception", "The address of main() was not found");
		}
		else
		{
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
		}
		if (machine.cpu.registers().pc == main_address)
		{
			// take a snapshot of the machine
			std::vector<uint8_t> program_state;
			machine.serialize_to(program_state);
			std::deque<uint64_t> samples;
			// begin benchmarking
			for (int i = 0; i < 1 + BENCH_SAMPLES; i++)
			{
				machine.deserialize_from(program_state);
				state.output.clear();
				uint64_t t0 = micros_now();
				asm("" : : : "memory");

				try {
					machine.simulate(MAX_INSTRUCTIONS);
					if (machine.cpu.instruction_counter() == MAX_INSTRUCTIONS) {
						res.set_header("X-Exception", "Maximum instructions reached");
						break;
					}
				} catch (std::exception& e) {
					res.set_header("X-Exception", e.what());
					break;
				}

				asm("" : : : "memory");
				const uint64_t t1 = micros_now();
				asm("" : : : "memory");
				samples.push_back(t1 - t0);
			}

			common_response_fields(res, 200);
			res.set_header("X-Exit-Code", std::to_string(state.exit_code));
			if (!samples.empty()) {
				const uint64_t first = samples[0];
				// we don't want to measure low/high/median on the first sample
				samples.pop_front();
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
			const size_t highest_mem = machine.memory.pages_highest_active() * 4096;
			res.set_header("X-Memory-Highest", std::to_string(highest_mem));
			const size_t max_mem = machine.memory.pages_total() * 4096;
			res.set_header("X-Memory-Max", std::to_string(highest_mem));
			res.set_content(state.output, "text/plain");
		}
		else {
			res.set_header("X-Exception", "Could not enter main()");
			res.set_header("X-Instruction-Count", std::to_string(MAX_INSTRUCTIONS));
		}
    });

	printf("Listening on %s:%u\n", ADDRESS, PORT);
    svr.listen(ADDRESS, PORT);
}

int create_folder(const std::string& folder)
{
	return mkdir(folder.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
}

int python_sanitize_compile(const std::string& pbase, const std::string& pdir, const std::string& method)
{
	auto cmd = "/usr/bin/python3 ../sanitize.py " + pbase + " " + pdir + " " + method;
	return system(cmd.c_str());
}

int write_file(const std::string& file, const std::string& text)
{
	FILE* fp = fopen(file.c_str(), "wb");
	if (fp == nullptr) return -1;
    fwrite(text.data(), text.size(), 1, fp);
    fclose(fp);
	return 0;
}
std::vector<uint8_t> load_file(const std::string& filename)
{
    size_t size = 0;
    FILE* f = fopen(filename.c_str(), "rb");
    if (f == NULL) return {};

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::vector<uint8_t> result(size);
    if (size != fread(result.data(), 1, size, f))
    {
        fclose(f);
        return {};
    }
    fclose(f);
    return result;
}

uint64_t micros_now()
{
	struct timespec ts;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return ts.tv_sec * 1000000ul + ts.tv_nsec / 1000ul;
}
