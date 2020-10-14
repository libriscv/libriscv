#include <libriscv/machine.hpp>

int main(int /*argc*/, const char** /*argv*/)
{
	// load binary from file
	const std::vector<uint8_t> binary /* = ... */;

	using namespace riscv;
	Machine<RISCV64> machine { binary };
	// install a system call handler
	machine.install_syscall_handler(93,
	 [] (auto& machine) {
		 const auto [code] = machine.template sysargs <int> ();
		 printf(">>> Program exited, exit code = %d\n", code);
		 machine.stop();
		 return 0;
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
