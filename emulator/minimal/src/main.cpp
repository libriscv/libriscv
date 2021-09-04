#include <libriscv/machine.hpp>

int main(int /*argc*/, const char** /*argv*/)
{
	// load binary from file
	const std::vector<uint8_t> binary /* = ... */;

	using namespace riscv;
	Machine<RISCV64> machine { binary };
	// install a system call handler
	Machine<RISCV64>::install_syscall_handler(93,
	 [] (Machine<RISCV64>& machine) {
		 const auto [code] = machine.sysargs <int> ();
		 printf(">>> Program exited, exit code = %d\n", code);
		 machine.stop();
	 });

	// add program arguments on the stack
	machine.setup_argv({"emulator", "test!"});

	// this function will run until the exit syscall has stopped the
	// machine, an exception happens which stops execution, or the
	// instruction counter reaches the given limit (1M):
	try {
		machine.simulate(1'000'000);
	} catch (const std::exception& e) {
		fprintf(stderr, ">>> Runtime exception: %s\n", e.what());
	}
}
