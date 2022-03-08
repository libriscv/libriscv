#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <vector>
#include <unistd.h>
#include "crc32.hpp"
static constexpr bool VERBOSE_COMPILER = true;
static const std::string DEFAULT_COMPILER = "riscv64-linux-gnu-gcc-10";

std::string compile_command(const std::string& cc,
	const std::string& args, const std::string& outfile)
{
	return cc + " -std=c11 -x c -o " + outfile + " " + args;
}
std::string env_with_default(const char* var, const std::string& defval) {
	std::string value = defval;
	if (const char* envval = getenv(var); envval) value = std::string(envval);
	return value;
}

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

std::vector<uint8_t> build_and_load(const std::string& code, const std::string& args)
{
	// Create temporary filenames for code and binary
	char code_filename[64];
	strncpy(code_filename, "/tmp/builder-XXXXXX", sizeof(code_filename));
	// Open temporary code file with owner privs
	const int code_fd = mkstemp(code_filename);
	if (code_fd < 0) {
		throw std::runtime_error(
			"Unable to create temporary file for code: " + std::string(code_filename));
	}
	// Write code to temp code file
	const ssize_t code_len = write(code_fd, code.c_str(), code.size());
	if (code_len < (ssize_t) code.size()) {
		unlink(code_filename);
		throw std::runtime_error("Unable to write to temporary file");
	}
	// Compile code to binary file
	char bin_filename[256];
	const uint32_t code_checksum = crc32(code.c_str(), code.size());
	const uint32_t final_checksum = crc32(code_checksum, args.c_str(), args.size());
	(void)snprintf(bin_filename, sizeof(bin_filename),
		"/tmp/binary-%08X", final_checksum);

	auto cc = env_with_default("cc", DEFAULT_COMPILER);
	auto command = compile_command(cc,
		std::string(code_filename) + " " + args, bin_filename);
	if constexpr (VERBOSE_COMPILER) {
		printf("Command: %s\n", command.c_str());
	}
	// Compile program
	FILE* f = popen(command.c_str(), "r");
	if (f == nullptr) {
		unlink(code_filename);
		throw std::runtime_error("Unable to compile code");
	}
	pclose(f);
	unlink(code_filename);

	return load_file(bin_filename);
}
