#include <catch2/catch_test_macros.hpp>

#include <libriscv/machine.hpp>
#include <dlfcn.h>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2 -static", bool cpp = false);

namespace {
struct TemporaryFile {
	std::string path;
	explicit TemporaryFile(const char *suffix) {
		std::string pattern = std::string("/tmp/libriscv-bintr-XXXXXX") + suffix;
		std::vector<char> writable(pattern.begin(), pattern.end());
		writable.push_back('\0');
		const int fd = mkstemps(writable.data(), std::char_traits<char>::length(suffix));
		REQUIRE(fd >= 0);
		close(fd);
		path = writable.data();
	}
	~TemporaryFile() { unlink(path.c_str()); }
};

void write_source(const std::string& path, const std::string& source) {
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	REQUIRE(file.is_open());
	file << source;
	REQUIRE(file.good());
}
}

TEST_CASE("Emit cache-loadable and embeddable C99 independently", "[Bintr]") {
	if (std::system("cc --version >/dev/null 2>&1") != 0) {
		SKIP("a system C compiler is not available");
	}

	const auto binary = build_and_load(R"(
		__attribute__((noinline)) int square(int value) { return value * value; }
		__asm__(".global _start\n"
		        "_start:\n"
		        "li a0, 7\n"
		        "call square\n"
		        "li a7, 1\n"
		        "ecall\n");
	)", "-O2 -static -ffreestanding -nostdlib -nostartfiles");
	std::string embeddable;
	std::string shared;
	riscv::MachineOptions<riscv::RISCV64> options;
	options.memory_max = 8u << 20;
	options.translate_enabled = false;
	options.translate_enable_embedded = false;
	options.translate_invoke_compiler = false;
	options.cross_compile.emplace_back(riscv::MachineTranslationEmbeddableCodeOptions{
		.result_c99 = &embeddable,
		.result_shared_c99 = &shared,
	});
	riscv::Machine<riscv::RISCV64> machine(binary, options);

	REQUIRE_FALSE(embeddable.empty());
	REQUIRE_FALSE(shared.empty());
	REQUIRE(embeddable.find("#define EMBEDDABLE_CODE 1") != std::string::npos);
	REQUIRE(shared.find("#define RISCV_TRANSLATION_DYLIB 8") != std::string::npos);

	TemporaryFile shared_c(".c");
	TemporaryFile shared_so(".so");
	write_source(shared_c.path, shared);
	const std::string shared_command = "cc -shared -fPIC -O2 -w -o " +
		shared_so.path + " " + shared_c.path;
	REQUIRE(std::system(shared_command.c_str()) == 0);
	void *shared_handle = dlopen(shared_so.path.c_str(), RTLD_NOW);
	REQUIRE(shared_handle != nullptr);
	REQUIRE(dlsym(shared_handle, "init") != nullptr);
	REQUIRE(dlsym(shared_handle, "no_mappings") != nullptr);
	dlclose(shared_handle);

	TemporaryFile embedded_c(".c");
	TemporaryFile embedded_so(".so");
	write_source(embedded_c.path, embeddable);
	const std::string embedded_command = "cc -shared -fPIC -O2 -w -DCALLBACK_INIT -o " +
		embedded_so.path + " " + embedded_c.path;
	REQUIRE(std::system(embedded_command.c_str()) == 0);
	void *embedded_handle = dlopen(embedded_so.path.c_str(), RTLD_NOW);
	REQUIRE(embedded_handle != nullptr);
	REQUIRE(dlsym(embedded_handle, "init") == nullptr);
	REQUIRE(dlsym(embedded_handle, "no_mappings") == nullptr);
	dlclose(embedded_handle);
}
