#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libriscv/machine.hpp>
#include <libriscv/debug.hpp>
extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2 -static", bool cpp = false);
static const uint64_t MAX_MEMORY = 8ul << 20; /* 8MB */
static const uint64_t MAX_INSTRUCTIONS = 10'000'000ul;
using namespace riscv;
static bool is_zig() {
	const char* rcc = getenv("RCC");
	if (rcc == nullptr)
		return false;
	return std::string(rcc).find("zig") != std::string::npos;
}

TEST_CASE("Calculate fib(2560000) on execute page", "[VA]")
{
	const auto binary = build_and_load(R"M(
#define uintptr_t __UINTPTR_TYPE__
typedef long (*fib_func)(long, long, long);

static long syscall(long n, long arg0);
static long syscall3(long n, long arg0, long arg1, long arg2);

static void copy(uintptr_t dst, const void *src, unsigned len) {
	for (unsigned i = 0; i < len; i++)
		((char *)dst)[i] = ((const char *)src)[i];
}

static long fib(long n, long acc, long prev)
{
	if (n == 0)
		return acc;
	else
		return fib(n - 1, prev + acc, acc);
}

int main()
{
	const uintptr_t DST = 0xF0000000;
	copy(DST, &fib, 256);
	// mprotect +execute
	syscall3(226, DST, 0x1000, 0x4);

	const volatile long n = 50;
	fib_func other_fib = (fib_func)DST;
	// exit(...)
	syscall(93, other_fib(n, 0, 1));
}

long syscall(long n, long arg0) {
	register long a0 __asm__("a0") = arg0;
	register long syscall_id __asm__("a7") = n;

	__asm__ volatile ("scall" : "+r"(a0) : "r"(syscall_id));

	return a0;
}
long syscall3(long n, long arg0, long arg1, long arg2) {
	register long a0 __asm__("a0") = arg0;
	register long a1 __asm__("a1") = arg1;
	register long a2 __asm__("a2") = arg2;
	register long syscall_id __asm__("a7") = n;

	__asm__ volatile ("scall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(syscall_id));

	return a0;
})M");

	static constexpr uint32_t VA_FUNC = 0xF0000000;

	// Normal (fastest) simulation
	{
		riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
		// We need to install Linux system calls for maximum gucciness
		machine.setup_linux_syscalls();
		// We need to create a Linux environment for runtimes to work well
		machine.setup_linux(
			{"va_exec"},
			{"LC_TYPE=C", "LC_ALL=C", "USER=root"});
		// Run for at most X instructions before giving up
		machine.simulate(MAX_INSTRUCTIONS);

		REQUIRE(machine.return_value<long>() == 12586269025L);

		// VM call into new execute segment
		REQUIRE(machine.vmcall(VA_FUNC, 50, 0, 1) == 12586269025L);
	}
	// Precise (step-by-step) simulation
	{
		riscv::Machine<RISCV64> machine{binary, { .memory_max = MAX_MEMORY }};
		machine.setup_linux_syscalls();
		machine.setup_linux(
			{"va_exec"},
			{"LC_TYPE=C", "LC_ALL=C", "USER=root"});
		// Verify step-by-step simulation
		machine.set_max_instructions(MAX_INSTRUCTIONS);
		machine.cpu.simulate_precise();

		REQUIRE(machine.return_value<long>() == 12586269025L);

		// VM call into new execute segment
		REQUIRE(machine.vmcall(VA_FUNC, 50, 0, 1) == 12586269025L);
	}
	// Debug-assisted simulation
	{
		riscv::Machine<RISCV64> machine{binary, {.memory_max = MAX_MEMORY}};
		machine.setup_linux_syscalls();
		machine.setup_linux(
			{"va_exec"},
			{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

		riscv::DebugMachine debugger { machine };
		//debugger.verbose_instructions = true;

		// Verify step-by-step simulation
		debugger.simulate(MAX_INSTRUCTIONS);

		REQUIRE(machine.return_value<long>() == 12586269025L);

		// VM call into new execute segment
		REQUIRE(machine.vmcall(VA_FUNC, 50, 0, 1) == 12586269025L);
	}
}

TEST_CASE("Calculate fib(50) on high-memory page", "[VA]")
{
	const auto binary = build_and_load(R"M(
	#include <stdlib.h>
	long fib(long n, long acc, long prev)
	{
		if (n < 1)
			return acc;
		else
			return fib(n - 1, prev + acc, acc);
	}
	int main(int argc, char** argv) {
		const long n = atoi(argv[1]);
		return fib(n, 0, 1);
	})M",
	is_zig() ? "-O2 -static -Wl,--image-base=0x20000000"
		: "-O2 -static -Wl,-Ttext-segment=0x20000000");

	// Normal (fastest) simulation
	{
		riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
		// We need to install Linux system calls for maximum gucciness
		machine.setup_linux_syscalls(false, false);
		// We need to create a Linux environment for runtimes to work well
		machine.setup_linux(
			{"va_exec", "50"},
			{"LC_TYPE=C", "LC_ALL=C", "USER=root"});
		// Run for at most X instructions before giving up
		machine.simulate(MAX_INSTRUCTIONS);

		REQUIRE(machine.return_value<int>() == -298632863);
	}
	// Precise (step-by-step) simulation
	{
		riscv::Machine<RISCV64> machine{binary, { .memory_max = MAX_MEMORY }};
		machine.setup_linux_syscalls(false, false);
		machine.setup_linux(
			{"va_exec", "50"},
			{"LC_TYPE=C", "LC_ALL=C", "USER=root"});
		// Verify step-by-step simulation
		machine.set_max_instructions(MAX_INSTRUCTIONS);
		machine.cpu.simulate_precise();

		REQUIRE(machine.return_value<int>() == -298632863);
	}
	// Debug-assisted simulation
	{
		riscv::Machine<RISCV64> machine{binary, {.memory_max = MAX_MEMORY}};
		machine.setup_linux_syscalls(false, false);
		machine.setup_linux(
			{"va_exec", "50"},
			{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

		riscv::DebugMachine debugger { machine };
		//debugger.verbose_instructions = true;

		// Verify step-by-step simulation
		debugger.simulate(MAX_INSTRUCTIONS);

		REQUIRE(machine.return_value<int>() == -298632863);
	}
}

// FENCE.I makes earlier stores to instruction memory visible to later fetches.
// Following the witness in issue #386: mmap an RWX page, hand-assemble
// `addi a0, x0, N; ret` into it, call it, rewrite the immediate, fence and call
// it again. Without segment invalidation the second call re-runs the bytecode
// decoded for the first one and still returns 1.
static const std::string SELFMOD_GUEST = R"M(
#define uintptr_t __UINTPTR_TYPE__
typedef int (*retfunc)(void);

static long syscall(long n, long arg0);
static long syscall6(long n, long a, long b, long c, long d, long e, long f);

int main()
{
	// mmap(NULL, 4096, PROT_READ|PROT_WRITE|PROT_EXEC,
	//      MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
	long page = syscall6(222, 0, 4096, 7, 34, -1, 0);
	if (page <= 0)
		syscall(93, 0);

	volatile unsigned *code = (volatile unsigned *)(uintptr_t)page;
	code[0] = 0x00100513; // addi a0, x0, 1
	code[1] = 0x00008067; // jalr x0, 0(ra)  == ret
	__asm__ volatile ("fence.i" ::: "memory");

	retfunc fn = (retfunc)(uintptr_t)page;
	int first = fn();

	code[0] = 0x00200513; // addi a0, x0, 2
	__asm__ volatile ("fence.i" ::: "memory");
	int second = fn();

	code[0] = 0x00300513; // addi a0, x0, 3
	__asm__ volatile ("fence.i" ::: "memory");
	int third = fn();

	// 1, 2, 3 => 123 on success. A stale decoder cache yields 111.
	syscall(93, first * 100 + second * 10 + third);
}

long syscall(long n, long arg0) {
	register long a0 __asm__("a0") = arg0;
	register long syscall_id __asm__("a7") = n;

	__asm__ volatile ("scall" : "+r"(a0) : "r"(syscall_id));

	return a0;
}
long syscall6(long n, long a, long b, long c, long d, long e, long f) {
	register long a0 __asm__("a0") = a;
	register long a1 __asm__("a1") = b;
	register long a2 __asm__("a2") = c;
	register long a3 __asm__("a3") = d;
	register long a4 __asm__("a4") = e;
	register long a5 __asm__("a5") = f;
	register long syscall_id __asm__("a7") = n;

	__asm__ volatile ("scall" : "+r"(a0)
		: "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(syscall_id));

	return a0;
})M";

TEST_CASE("FENCE.I observes stores to instruction memory", "[SelfModify]")
{
	const auto binary = build_and_load(SELFMOD_GUEST);

	// Bytecode dispatch (threaded/switch/tailcall, plus binary translation
	// when it is compiled in)
	{
		riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
		machine.setup_linux_syscalls();
		machine.setup_linux({"selfmod"}, {"LC_TYPE=C", "LC_ALL=C", "USER=root"});
		machine.simulate(MAX_INSTRUCTIONS);

		REQUIRE(machine.return_value<long>() == 123L);
	}
	// Step-by-step simulation shares the decoder cache but not the dispatch loop
	{
		riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
		machine.setup_linux_syscalls();
		machine.setup_linux({"selfmod"}, {"LC_TYPE=C", "LC_ALL=C", "USER=root"});
		machine.set_max_instructions(MAX_INSTRUCTIONS);
		machine.cpu.simulate_precise();

		REQUIRE(machine.return_value<long>() == 123L);
	}
	// Inaccurate dispatch (no instruction counting)
	{
		riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
		machine.setup_linux_syscalls();
		machine.setup_linux({"selfmod"}, {"LC_TYPE=C", "LC_ALL=C", "USER=root"});
		machine.simulate<false>(MAX_INSTRUCTIONS);

		REQUIRE(machine.return_value<long>() == 123L);
	}
}
