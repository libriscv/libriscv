#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>
#include "crc32.hpp"
static constexpr bool VERBOSE_COMPILER = true;
// The guest is a normal Linux program: it is statically linked against glibc
// so that libriscv can load it directly, and it brings its own global
// allocator that forwards to the host arena (see native_rust.cpp).
static const std::string RUST_TARGET = "riscv64gc-unknown-linux-gnu";
static const std::string DEFAULT_RUSTC = "rustc";
// Newest first: rustc only needs a cross-gcc to drive the link, so any of
// these will do, but the list should stay in sync with the compilers that
// codebuilder.cpp uses for the C and C++ guests
static const std::vector<std::string> RUST_LINKER_CANDIDATES {
	"riscv64-linux-gnu-gcc-14",
	"riscv64-linux-gnu-gcc-13",
	"riscv64-linux-gnu-gcc-12",
	"riscv64-linux-gnu-gcc",
};

// Both defined in codebuilder.cpp
extern std::vector<uint8_t> load_file(const std::string& filename);
extern std::string env_with_default(const char* var, const std::string& defval);

static std::string command_output(const std::string& command)
{
	FILE* f = popen((command + " 2>/dev/null").c_str(), "r");
	if (f == nullptr)
		return {};

	std::string output;
	char buffer[512];
	size_t len;
	while ((len = fread(buffer, 1, sizeof(buffer), f)) > 0)
		output.append(buffer, len);

	if (pclose(f) != 0)
		return {};
	while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
		output.pop_back();
	return output;
}

static bool command_succeeds(const std::string& command)
{
	FILE* f = popen((command + " >/dev/null 2>&1").c_str(), "r");
	if (f == nullptr)
		return false;
	return pclose(f) == 0;
}

/// @brief The cross-gcc that rustc will use to link the guest, or an empty
/// string when this machine has none of them.
static std::string rust_linker()
{
	if (const char* envval = getenv("RUST_LINKER"); envval)
		return std::string(envval);

	for (const auto& candidate : RUST_LINKER_CANDIDATES) {
		if (command_succeeds("command -v " + candidate))
			return candidate;
	}
	return {};
}

/// @brief True when this machine can build the RISC-V Rust guest: a rustc
/// that has the riscv64gc standard library installed, and a cross-linker.
bool rust_toolchain_available()
{
	const auto rustc  = env_with_default("RUSTC", DEFAULT_RUSTC);
	const auto linker = rust_linker();

	if (!command_succeeds(rustc + " --version"))
		return false;
	if (linker.empty() || !command_succeeds("command -v " + linker))
		return false;

	// rustc prints the target library path whether or not it was installed,
	// so the standard library itself is what decides
	const auto libdir = command_output(
		rustc + " --print target-libdir --target " + RUST_TARGET);
	if (libdir.empty())
		return false;
	return command_succeeds("ls " + libdir + "/libstd-*.rlib");
}

static bool file_exists(const std::string& filename)
{
	return access(filename.c_str(), R_OK) == 0;
}

std::vector<uint8_t> build_rust_and_load(
	const std::string& code, const std::string& args)
{
	const auto rustc  = env_with_default("RUSTC", DEFAULT_RUSTC);
	const auto linker = rust_linker();

	// The name of the compiled guest is a checksum of everything that goes into
	// it, so a binary that is already there is the binary this call would have
	// produced. Building the Rust guest takes several seconds and every test
	// binary that uses it needs the same one, so it is only built once.
	// --no-gc-sections keeps the exported functions in the binary even though
	// nothing in the guest itself calls them
	const std::string command_prefix =
		rustc + " --edition 2021 --crate-name rust_guest --target " + RUST_TARGET
		+ " -C target-feature=+crt-static"
		+ " -C linker=" + linker
		+ " -C link-args=-Wl,--no-gc-sections "
		+ args;

	uint32_t checksum = crc32((const uint8_t *)code.c_str(), code.size());
	checksum = crc32(checksum, (const uint8_t *)command_prefix.c_str(), command_prefix.size());

	char bin_filename[256];
	(void)snprintf(bin_filename, sizeof(bin_filename),
		"/tmp/rustbinary-%08X", checksum);

	if (file_exists(bin_filename))
		return load_file(bin_filename);

	// Create a temporary source file. rustc wants the .rs extension, and it
	// takes the crate name from the file name, which mkstemps does not make
	// a valid Rust identifier - hence --crate-name above.
	char code_filename[64];
	strncpy(code_filename, "/tmp/rustbuilder-XXXXXX.rs", sizeof(code_filename));
	const int code_fd = mkstemps(code_filename, 3);
	if (code_fd < 0) {
		throw std::runtime_error(
			"Unable to create temporary file for code: " + std::string(code_filename));
	}
	const ssize_t code_len = write(code_fd, code.c_str(), code.size());
	close(code_fd);
	if (code_len < (ssize_t) code.size()) {
		unlink(code_filename);
		throw std::runtime_error("Unable to write to temporary file");
	}

	// Link into a private file and rename it into place afterwards, so that
	// test binaries running side by side never load a half-written guest.
	const std::string temp_binary =
		std::string(bin_filename) + "." + std::to_string(getpid());

	const std::string command = command_prefix
		+ " -o " + temp_binary + " " + std::string(code_filename);

	if constexpr (VERBOSE_COMPILER) {
		printf("Command: %s\n", command.c_str());
	}
	FILE* f = popen(command.c_str(), "r");
	if (f == nullptr) {
		unlink(code_filename);
		throw std::runtime_error("Unable to compile Rust code");
	}
	pclose(f);
	unlink(code_filename);

	if (rename(temp_binary.c_str(), bin_filename) != 0) {
		unlink(temp_binary.c_str());
		throw std::runtime_error("Unable to build the Rust guest: " + command);
	}

	return load_file(bin_filename);
}
