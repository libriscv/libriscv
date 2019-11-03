#include <httplib.h>
#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <array>
#include <libriscv/machine.hpp>
#include "syscalls.cpp"

static const char* ADDRESS = "localhost";
static const uint16_t PORT = 1234;
// avoid endless loops and code that takes too long
static const uint32_t MAX_INSTRUCTIONS = 40000;

static int create_folder(const std::string& folder);
static int python_sanitize_compile(const std::string& project, const std::string& outfile);
static int write_file(const std::string& file, const std::string& text);
static std::vector<uint8_t> load_file(const std::string& filename);

int main(void)
{
    using namespace httplib;
    Server svr;

    svr.Post("/exec", [](const Request& req, Response& res) {
		static int request_ID = 0;
		const std::string project_folder = "program" + std::to_string(request_ID++);
		const std::string codefile   = project_folder + "/code.cpp";
		const std::string binaryfile = project_folder + "/binary";

		// create project folder
		if (create_folder(project_folder) < 0) {
			if (errno != EEXIST) {
				res.set_content("Failed to create project folder", "text/plain");
				res.status = 500;
				return;
			}
		}

		// write code into project folder
		if (write_file(codefile, req.body) != 0) {
			res.set_content("Failed to write codefile", "text/plain");
			res.status = 500;
			return;
		}

		// sanitize + compile code
		const int cc = python_sanitize_compile(project_folder, binaryfile);
		if (cc != 0) {
			auto vec = load_file(project_folder + "/status.txt");
			res.set_content((const char*) vec.data(), vec.size(), "text/plain");
			res.status = 500;
			return;
		}

		// load binary and execute code
		//printf("Loading binary: %s\n", binaryfile.c_str());
		auto binary = load_file(binaryfile);
		if (binary.empty()) {
			res.set_content("Failed to open binary", "text/plain");
			res.status = 500;
			return;
		}
		State<4> state;
		riscv::Machine<4> machine { binary };
		machine.install_syscall_handler(64, {&state, &State<4>::syscall_write});
		machine.install_syscall_handler(93, {&state, &State<4>::syscall_exit});
		machine.install_syscall_handler(214, {&state, &State<4>::syscall_brk});
		try {
			while (!machine.stopped()) {
				machine.simulate();
				if (machine.cpu.registers().counter > MAX_INSTRUCTIONS) break;
			}
		} catch (std::exception& e) {
			res.set_content("Exception: " + std::string(e.what()), "text/plain");
			res.status = 500;
			return;
		}

        res.set_content(state.output, "text/plain");
		res.set_header("X-Exit-Code", std::to_string(state.exit_code));
		res.status = 200;
    });

	printf("Listening on %s:%u\n", ADDRESS, PORT);
    svr.listen(ADDRESS, PORT);
}

int create_folder(const std::string& folder)
{
	return mkdir(folder.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
}

int python_sanitize_compile(const std::string& project, const std::string& outfile)
{
	const std::string cmd = "/usr/bin/python3 ../sanitize.py " + project + " " + outfile;
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
