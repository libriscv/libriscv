#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libriscv/machine.hpp>
#include <libriscv/debug.hpp>
extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2 -static", bool cpp = false);
static const std::string cwd {SRCDIR};
using namespace riscv;
static bool is_zig() {
	const char* rcc = getenv("RCC");
	if (rcc == nullptr)
		return false;
	return std::string(rcc).find("zig") != std::string::npos;
}

TEST_CASE("Forked machines require an empty flat memory arena", "[Fork]")
{
	if constexpr (!riscv::flat_readwrite_arena)
		return;

	Machine<RISCV64> master {{
		.memory_max = 8ul << 20,
		.use_memory_arena = true,
	}};
	REQUIRE(master.memory.uses_flat_memory_arena());

	// The master may use an arena as long as the fork does not inherit it.
	Machine<RISCV64> fork {master, {
		.use_memory_arena = false,
	}};
	REQUIRE(fork.memory.memory_arena_ptr() == nullptr);
	REQUIRE(fork.memory.memory_arena_size() == 0);

	REQUIRE_THROWS_AS(Machine<RISCV64>(master, {
		.use_memory_arena = true,
	}), MachineException);
}

/**
 * These tests are designed to be really brutal to support,
 * and most emulators will surely fail here.
*/

TEST_CASE("Calculate fib(50) slowly, basic", "[Compute]")
{
	const auto binary = build_and_load(R"M(
	long fib(long n, long acc, long prev)
	{
		if (n < 1)
			return acc;
		else
			return fib(n - 1, prev + acc, acc);
	}
	__attribute__((used, retain))
	extern long my_start(long n) {
		return fib(n, 0, 1);
	}
	int main() {}
	)M", "-O2 -static");

	riscv::Machine<RISCV64> machine { binary, { .use_memory_arena = false } };

	const auto addr = machine.address_of("my_start");
	REQUIRE(addr != 0x0);

	// Create a manual VM call in order to avoid exercising the C-runtime
	// The goal is to see if the basic start/stop/resume functionality works
	machine.cpu.jump(addr);
	machine.cpu.reg(riscv::REG_ARG0) = 50;
	machine.cpu.reg(riscv::REG_RA) = machine.memory.exit_address();

	riscv::Machine<RISCV64> fork { machine };
	do {
		// No matter how many (or few) instructions we execute before exiting
		// simulation, we should be able to resume and complete the program normally.
		for (int step = 5; step < 105; step++) {
			fork.cpu.registers() = machine.cpu.registers();
			do {
				fork.simulate<false>(step);
			} while (fork.instruction_limit_reached());
			REQUIRE(fork.return_value<long>() == 12586269025L);
		}
		machine.simulate<false>(100);
	} while (machine.instruction_limit_reached());
}

TEST_CASE("Execute libc_start_main, slowly", "[Compute]")
{
	const auto binary = build_and_load(R"M(
	int main() {
		return 1234;
	})M");

	riscv::Machine<RISCV64> machine { binary, { .use_memory_arena = false } };
	machine.setup_linux_syscalls();
	machine.setup_linux(
		{"brutal", "50"},
		{"LC_TYPE=C", "LC_ALL=C"});

	do {
		// No matter how many (or few) instructions we execute before exiting
		// simulation, we should be able to resume and complete the program normally.
		for (int step = 5; step < 105; step++) {
			riscv::Machine<RISCV64> fork { machine, { .use_memory_arena = false } };
			do {
				fork.simulate<false>(step);
			} while (fork.instruction_limit_reached());
			REQUIRE(fork.return_value<long>() == 1234);
		}
		machine.simulate<false>(1000);
	} while (machine.instruction_limit_reached());

	REQUIRE(machine.return_value<long>() == 1234);
}

TEST_CASE("Threads test-suite slowly", "[Compute]")
{
	if (is_zig())
		return;

	const auto binary = build_and_load(R"M(
	#include "threads/test_threads.cpp"
	)M", "-O1 -static -pthread -I" + cwd, true);

	riscv::Machine<RISCV64> machine { binary, { .use_memory_arena = false } };
	machine.setup_linux_syscalls();
	machine.setup_posix_threads();
	machine.setup_linux(
		{"brutal", "123"},
		{"LC_TYPE=C", "LC_ALL=C"});

	do {
		// No matter how many (or few) instructions we execute before exiting
		// simulation, we should be able to resume and complete the program normally.
		const int step = 5;
		riscv::Machine<RISCV64> fork { machine };
		fork.set_printer([] (const auto&, const char*, size_t) {});

		do {
			fork.simulate<false>(step);
		} while (fork.instruction_limit_reached());
		REQUIRE(fork.return_value<long>() == 123666123L);

		machine.simulate<false>(100000UL);
	} while (machine.instruction_limit_reached());
	REQUIRE(machine.return_value<long>() == 123666123L);
}

/// Without a flat arena every access goes through the page tables. With asmjit
/// that means the inline page-cache probe, which these tests are for, so make
/// sure it is actually the code that runs.
static void require_paged_translation(const riscv::Machine<RISCV64>& machine)
{
	REQUIRE(!machine.memory.uses_flat_memory_arena());
	if constexpr (riscv::asmjit_enabled)
		REQUIRE(machine.cpu.current_execute_segment().is_asmjit_translated());
}

TEST_CASE("Forks see their own copy-on-write pages through the page caches", "[Fork]")
{
	// Four arrays, two pages each, visited interleaved so that every access
	// moves to another page. The fork first reads the master's pages, then
	// writes them (copying each one), then reads them again: a page cache
	// left pointing at the master's data would miss every write.
	const auto binary = build_and_load(R"M(
	#define N 1024
	static volatile long g[4][N];

	__attribute__((used, retain, noinline))
	long sum_all() {
		long s = 0;
		for (int i = 0; i < N; i += 7)
			for (int a = 0; a < 4; a++)
				s += g[a][i];
		return s;
	}
	__attribute__((used, retain))
	long bump(long v) {
		const long before = sum_all();
		for (int i = 0; i < N; i += 7)
			for (int a = 0; a < 4; a++)
				g[a][i] += v;
		return sum_all() - before;
	}

	int main() {
		for (int a = 0; a < 4; a++)
			for (int i = 0; i < N; i++)
				g[a][i] = i * (a + 1);
		return 0;
	})M");

	riscv::Machine<RISCV64> machine { binary, { .use_memory_arena = false } };
	require_paged_translation(machine);
	machine.setup_linux_syscalls();
	machine.setup_linux({"brutal"}, {"LC_TYPE=C", "LC_ALL=C"});
	machine.simulate();
	REQUIRE(machine.return_value<long>() == 0);

	constexpr long ACCESSES = 4 * ((1024 + 6) / 7);
	const long master_sum = machine.vmcall("sum_all");

	for (long v = 1; v <= 4; v++) {
		riscv::Machine<RISCV64> fork { machine, { .use_memory_arena = false } };
		REQUIRE(fork.vmcall("sum_all") == master_sum);
		REQUIRE(fork.vmcall("bump", v) == v * ACCESSES);
		REQUIRE(fork.vmcall("bump", v) == v * ACCESSES);
		REQUIRE(fork.vmcall("sum_all") == master_sum + 2 * v * ACCESSES);
		// The master's pages are untouched.
		REQUIRE(machine.vmcall("sum_all") == master_sum);
	}
	// Writes by the master itself go through the same caches.
	REQUIRE(machine.vmcall("bump", 10) == 10 * ACCESSES);
	REQUIRE(machine.vmcall("sum_all") == master_sum + 10 * ACCESSES);
}

/// One page per slot. Each function touches the first word of a slot's page,
/// so that the page caches hold it, then changes the page underneath them.
static const char* page_cache_program = R"M(
	#define N 512
	static volatile long g[4][N] __attribute__((aligned(4096)));

	static long sys6(long n, long a, long b, long c, long d, long e, long f) {
		register long a0 __asm__("a0") = a;
		register long a1 __asm__("a1") = b;
		register long a2 __asm__("a2") = c;
		register long a3 __asm__("a3") = d;
		register long a4 __asm__("a4") = e;
		register long a5 __asm__("a5") = f;
		register long a7 __asm__("a7") = n;
		__asm__ volatile("ecall" : "+r"(a0)
			: "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7) : "memory");
		return a0;
	}

	__attribute__((used, retain))
	long page_addr(long a) { return (long)&g[a][0]; }
	__attribute__((used, retain))
	long read_at(long a) { return g[a][0]; }
	__attribute__((used, retain))
	void write_at(long a, long v) { g[a][0] = v; }

	__attribute__((used, retain))
	long protect_then_write(long a, long prot) {
		g[a][0] = 5;
		if (sys6(226, (long)&g[a][0], 4096, prot, 0, 0, 0) != 0)
			return -1;
		g[a][0] = 6;
		return g[a][0];
	}
	__attribute__((used, retain))
	long read_then_madvise(long a) {
		if (g[a][0] == 0)
			return -1;
		if (sys6(233, (long)&g[a][0], 4096, 4 /* MADV_DONTNEED */, 0, 0, 0) != 0)
			return -1;
		return g[a][0];
	}
	__attribute__((used, retain))
	long read_then_mmap_noreserve(long a) {
		if (g[a][0] == 0)
			return -1;
		/* MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS | MAP_NORESERVE */
		if (sys6(222, (long)&g[a][0], 4096, 3, 0x02 | 0x10 | 0x20 | 0x4000, -1, 0)
				!= (long)&g[a][0])
			return -1;
		return g[a][0];
	}

	int main() {
		for (int a = 0; a < 4; a++)
			g[a][0] = 100 + a;
		return 0;
	})M";

static void boot_page_cache_program(riscv::Machine<RISCV64>& machine)
{
	require_paged_translation(machine);
	machine.setup_linux_syscalls();
	machine.setup_linux({"brutal"}, {"LC_TYPE=C", "LC_ALL=C"});
	machine.simulate();
	REQUIRE(machine.return_value<long>() == 0);
}

TEST_CASE("Page caches drop pages whose protections change", "[Memory]")
{
	const auto binary = build_and_load(page_cache_program);
	riscv::Machine<RISCV64> machine { binary, { .use_memory_arena = false } };
	boot_page_cache_program(machine);
	const auto page0 = machine.vmcall("page_addr", 0);

	// The host changes the protections of a page the guest has just written
	machine.vmcall("write_at", 0, 1);
	REQUIRE(machine.vmcall("read_at", 0) == 1);
	machine.memory.set_page_attr(page0, 4096, { .read = true, .write = false });
	REQUIRE_THROWS_AS(machine.vmcall("write_at", 0, 2), MachineException);
	REQUIRE(machine.vmcall("read_at", 0) == 1);
	machine.memory.set_page_attr(page0, 4096, { .read = false, .write = false });
	REQUIRE_THROWS_AS(machine.vmcall("read_at", 0), MachineException);
	machine.memory.set_page_attr(page0, 4096, {});
	machine.vmcall("write_at", 0, 3);
	REQUIRE(machine.vmcall("read_at", 0) == 3);

	// The guest does it with mprotect, between two writes to the page
	REQUIRE_THROWS_AS(machine.vmcall("protect_then_write", 1, 1 /* PROT_READ */), MachineException);
	REQUIRE(machine.vmcall("read_at", 1) == 5);
	REQUIRE_THROWS_AS(machine.vmcall("write_at", 1, 7), MachineException);
	REQUIRE(machine.vmcall("protect_then_write", 2, 3 /* PROT_READ | PROT_WRITE */) == 6);
	REQUIRE(machine.vmcall("read_at", 2) == 6);
}

TEST_CASE("Discarding a fork's copy-on-write page drops it from the page caches", "[Fork]")
{
	const auto binary = build_and_load(page_cache_program);
	riscv::Machine<RISCV64> machine { binary, { .use_memory_arena = false } };
	boot_page_cache_program(machine);

	for (int i = 0; i < 2; i++) {
		riscv::Machine<RISCV64> fork { machine, { .use_memory_arena = false } };
		// The fork reads the master's page, and then copies it to discard it
		REQUIRE(fork.vmcall("read_then_madvise", 1) == 0);
		REQUIRE(fork.vmcall("read_at", 1) == 0);
		REQUIRE(fork.vmcall("read_then_mmap_noreserve", 2) == 0);
		REQUIRE(fork.vmcall("read_at", 2) == 0);
		// The master's pages are untouched
		REQUIRE(machine.vmcall("read_at", 1) == 101);
		REQUIRE(machine.vmcall("read_at", 2) == 102);
	}
}

TEST_CASE("Trapping a cached page, and mprotect of a trapped page", "[Memory Traps]")
{
	if constexpr (!riscv::memory_traps_enabled)
		return;
	const auto binary = build_and_load(page_cache_program);
	riscv::Machine<RISCV64> machine { binary, { .use_memory_arena = false } };
	boot_page_cache_program(machine);

	// Both page caches hold the page before the trap is installed
	machine.vmcall("write_at", 3, 7);
	REQUIRE(machine.vmcall("read_at", 3) == 7);

	int reads = 0, writes = 0;
	machine.memory.trap(machine.vmcall("page_addr", 3),
		[&] (Page&, uint32_t, int mode, int64_t) {
			if (Page::trap_mode(mode) == TRAP_READ) reads++;
			else if (Page::trap_mode(mode) == TRAP_WRITE) writes++;
		});
	REQUIRE(machine.vmcall("read_at", 3) == 7);
	machine.vmcall("write_at", 3, 8);
	REQUIRE((reads == 1 && writes == 1));

	// mprotect must not make the page cacheable again
	REQUIRE(machine.vmcall("protect_then_write", 3, 3 /* PROT_READ | PROT_WRITE */) == 7);
	REQUIRE((reads == 2 && writes == 3));
	machine.vmcall("read_at", 3);
	REQUIRE(reads == 3);
}
