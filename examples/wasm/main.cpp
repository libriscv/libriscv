#include <libriscv/machine.hpp>
#include <chrono>
using Machine = riscv::Machine<riscv::RISCV64>;
static const std::vector<uint8_t> empty;
static constexpr uint64_t MAX_CYCLES = 15'000'000'000ull;
#define TIME_POINT(t) \
	asm("" ::: "memory"); \
	auto t = std::chrono::high_resolution_clock::now(); \
	asm("" ::: "memory");
static const uint8_t program_elf[] = {
#embed "program/program.elf"
};

int main()
{
	std::string_view binview{(const char *)program_elf, sizeof(program_elf)};
	Machine machine { binview };
	machine.setup_linux_syscalls();
	machine.setup_posix_threads();
	machine.setup_linux(
		{"libriscv", "Hello", "World"},
		{"LC_ALL=C", "USER=groot"}
	);

	try {
		// Initialize LuaJIT
		machine.simulate(MAX_CYCLES);
	} catch (const std::exception& e) {
		fprintf(stderr, ">>> Exception: %s\n", e.what());
	}

	TIME_POINT(t0);
	// Run LuaJIT script
	machine.vmcall("run", R"V0G0N(
		print("Hello, WebAssembly!")
		function fib(n, acc, prev)
			if (n < 1) then
				return acc
			else
				return fib(n - 1, prev + acc, acc)
			end
		end
		print("The 500th fibonacci number is " .. fib(500, 0, 1))
		return 42
	)V0G0N");
	std::string result = machine.return_value<std::string>();
	TIME_POINT(t1);

	const std::chrono::duration<double, std::milli> exec_time = t1 - t0;
	printf("\nRuntime: %.3fms  Result: %s\n",
		exec_time.count(), result.c_str());
	if (machine.memory.execute_segments_count() > 1) {
		printf(">>> Multiple execute segments detected, "
			"this means the JIT likely was activated!\n");
	}
}
