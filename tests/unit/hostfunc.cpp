#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libriscv/machine.hpp>
extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2", bool cpp = false);
static const uint64_t MAX_MEMORY = 8ul << 20; /* 8MB */
static const uint64_t MAX_INSTRUCTIONS = 10'000'000ul;
using namespace riscv;

TEST_CASE("Dynamic host functions", "[HostFunctions]")
{
	struct State {
		std::string text;
	} state;
	const auto binary = build_and_load(R"M(
	extern long host_write(const void*, unsigned);
	extern void hello() {
		host_write("Hello vmcall World!", 20);
	}

	int main() {
		host_write("Hello Main World!", 18);
		return 666;
	})M");

	using Machine = riscv::Machine<RISCV64>;
	using address_t = riscv::address_type<riscv::RISCV64>;

	Machine::RegisterHostFunction("host_write", [] (Machine& m) {
		auto [addr, len] = m.template sysargs<address_t, unsigned>();
		auto* state = m.template get_userdata<State> ();
		state->text = m.memory.memstring(addr, len);
		m.set_result(len);
	});

	Machine machine { binary, { .memory_max = MAX_MEMORY } };
	// We need to install Linux system calls for maximum gucciness
	machine.setup_linux_syscalls();
	// We need to create a Linux environment for runtimes to work well
	machine.setup_linux(
		{"hostfunc"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	machine.set_userdata(&state);
	// Run for at most X instructions before giving up
	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(machine.return_value<int>() == 666);
	REQUIRE(state.text == "Hello Main World!");

	const auto hello_address = machine.address_of("hello");
	REQUIRE(hello_address != 0x0);

	// Execute guest function
	machine.vmcall(hello_address);
	REQUIRE(state.text == "Hello vmcall World!");
}
