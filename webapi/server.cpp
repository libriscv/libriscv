#include <httplib.h>
#include <cstdio>
#include <sys/time.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <array>
#include <libriscv/machine.hpp>
#include "syscalls.cpp"
#include "webpage.h"

static const char* ADDRESS = "0.0.0.0";
static const uint16_t PORT = 1234;
// avoid endless loops and code that takes too long
static const size_t MAX_INSTRUCTIONS = 256000;

static int create_folder(const std::string& folder);
static int python_sanitize_compile(const std::string& project, const std::string& infile, const std::string& outfile);
static int write_file(const std::string& file, const std::string& text);
static std::vector<uint8_t> load_file(const std::string& filename);
static uint64_t micros_now();

inline void common_response_fields(httplib::Response& res, int status)
{
	res.status = status;
	res.set_header("Access-Control-Allow-Origin", "*");
	res.set_header("Access-Control-Expose-Headers", "*");
}

int main(void)
{
    using namespace httplib;
    Server svr;

	svr.Get("/", [](const Request& req, Response& res) {
		res.set_content((const char*) webpage_html, webpage_html_len, "text/html");
		res.status = 200;
	});

    svr.Post("/exec", [](const Request& req, Response& res) {
		static int request_ID = 0;
		const std::string project_folder = "program" + std::to_string(request_ID++);
		const std::string codefile   = "code.cpp";
		const std::string binaryfile = "binary";

		// create project folder
		if (create_folder(project_folder) < 0) {
			if (errno != EEXIST) {
				common_response_fields(res, 200);
				res.set_header("X-Error", "Failed to create project folder");
				return;
			}
		}

		// write code into project folder
		if (write_file(project_folder + "/" + codefile, req.body) != 0) {
			common_response_fields(res, 200);
			res.set_header("X-Error", "Failed to write codefile");
			return;
		}

		// sanitize + compile code
		const uint64_t c0 = micros_now();
		const int cc = python_sanitize_compile(project_folder, codefile, binaryfile);
		if (cc != 0) {
			common_response_fields(res, 200);
			auto vec = load_file(project_folder + "/status.txt");
			res.set_header("X-Error", "Compilation failed");
			res.set_content((const char*) vec.data(), vec.size(), "text/plain");
			return;
		}
		const uint64_t c1 = micros_now();

		// load binary and execute code
		auto binary = load_file(project_folder + "/" + binaryfile);
		if (binary.empty()) {
			common_response_fields(res, 200);
			res.set_header("X-Error", "Failed to open binary");
			return;
		}

		// start out with this:

		// go-time: create machine, execute code
		const uint64_t t0 = micros_now();
		State<4> state;
		riscv::Machine<4> machine { binary };
		machine.install_syscall_handler(64, {&state, &State<4>::syscall_write});
		machine.install_syscall_handler(80, {&state, &State<4>::syscall_dummy});
		machine.install_syscall_handler(93, {&state, &State<4>::syscall_exit});
		machine.install_syscall_handler(214, {&state, &State<4>::syscall_brk});

		try {
			while (!machine.stopped()) {
				machine.simulate();
				if (UNLIKELY(machine.cpu.registers().counter >= MAX_INSTRUCTIONS)) {
					res.set_header("X-Exception", "Maximum instructions reached");
					break;
				}
			}
		} catch (std::exception& e) {
			res.set_header("X-Exception", e.what());
		}
		const uint64_t t1 = micros_now();
		const auto instructions = std::to_string(machine.cpu.registers().counter);

		common_response_fields(res, 200);
		res.set_header("X-Exit-Code", std::to_string(state.exit_code));
		res.set_header("X-Compile-Time", std::to_string(c1 - c0) + " micros");
		res.set_header("X-Execution-Time", std::to_string(t1 - t0) + " micros");
		res.set_header("X-Instruction-Count", instructions);
		res.set_content(state.output, "text/plain");
    });

	printf("Listening on %s:%u\n", ADDRESS, PORT);
    svr.listen(ADDRESS, PORT);
}

int create_folder(const std::string& folder)
{
	return mkdir(folder.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
}

int python_sanitize_compile(const std::string& project, const std::string& infile, const std::string& outfile)
{
	const std::string cmd = "/usr/bin/python3 ../sanitize.py " + project + " " + infile + " " + outfile;
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
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000000ul + tv.tv_usec;
}
