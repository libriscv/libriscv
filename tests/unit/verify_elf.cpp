#include <catch2/catch_test_macros.hpp>
#include <libriscv/machine.hpp>
extern std::vector<uint8_t> load_file(const std::string& filename);
static const uint64_t MAX_MEMORY = 8ul << 20; /* 8MB */
static const uint64_t MAX_INSTRUCTIONS = 10'000'000ul;
static const std::string cwd {SRCDIR};
using namespace riscv;

TEST_CASE("Golang Hello World", "[Verify]")
{
	const auto binary = load_file(cwd + "/elf/golang-riscv64-hello-world");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	// We need to install Linux system calls for maximum gucciness
	machine.setup_linux_syscalls();
	machine.fds().permit_filesystem = true;
	machine.fds().permit_sockets = false;
	machine.fds().filter_open = [] (void* user, const std::string& path) {
		(void) user; (void) path;
		return false;
	};
	// multi-threading
	machine.setup_posix_threads();
	// We need to create a Linux environment for runtimes to work well
	machine.setup_linux(
		{"golang-riscv64-hello-world"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	struct State {
		bool output_is_hello_world = false;
	} state;
	machine.set_userdata(&state);
	machine.set_printer([] (const auto& m, const char* data, size_t size) {
		auto* state = m.template get_userdata<State> ();
		std::string text{data, data + size};
		state->output_is_hello_world = (text == "hello world");
	});

	// Run for at most X instructions before giving up
	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(machine.return_value() == 0);
	REQUIRE(state.output_is_hello_world);
}

TEST_CASE("Zig Hello World", "[Verify]")
{
	const auto binary = load_file(cwd + "/elf/zig-riscv64-hello-world");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	// Install Linux system calls
	machine.setup_linux_syscalls();
	// Create a Linux environment for runtimes to work well
	machine.setup_linux(
		{"zig-riscv64-hello-world"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	struct State {
		std::string text;
	} state;
	machine.set_userdata(&state);
	machine.set_printer([] (const auto& m, const char* data, size_t size) {
		auto* state = m.template get_userdata<State> ();
		state->text.append(data, data + size);
	});

	// Run for at most X instructions before giving up
	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(machine.return_value() == 0);
	REQUIRE(state.text == "Hello, world!\n");
}

TEST_CASE("Rust Hello World", "[Verify]")
{
	const auto binary = load_file(cwd + "/elf/rust-riscv64-hello-world");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	// Install Linux system calls
	machine.setup_linux_syscalls();
	machine.setup_posix_threads();
	// Create a Linux environment for runtimes to work well
	machine.setup_linux(
		{"rust-riscv64-hello-world"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	struct State {
		std::string text;
	} state;
	machine.set_userdata(&state);
	machine.set_printer([] (const auto& m, const char* data, size_t size) {
		auto* state = m.template get_userdata<State> ();
		state->text.append(data, data + size);
	});

	// Run for at most X instructions before giving up
	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(machine.return_value() == 0);
	REQUIRE(state.text == "Hello World!\n");
}
