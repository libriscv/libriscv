#include <catch2/catch_test_macros.hpp>

#include <libriscv/machine.hpp>
extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2 -static", bool cpp = false);
static const uint64_t MAX_MEMORY = 32ul << 20;
static const uint64_t MAX_INSTRUCTIONS = 20'000'000ul;
using namespace riscv;

TEST_CASE("Guest JIT publishes code with riscv_flush_icache", "[SMC]")
{
	// A guest JIT writes a function, publishes it the way
	// __builtin___clear_cache() does, and then calls it. Overwriting the
	// function and publishing it again must not run the first version.
	const auto binary = build_and_load(R"M(
	#include <sys/mman.h>
	extern long syscall(long, ...);

	static void publish(void *begin, void *end) {
		syscall(259 /* riscv_flush_icache */, begin, end, 0);
	}

	int main() {
		unsigned int *code = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (code == MAP_FAILED)
			return -1;

		/* li a0, 111; ret */
		code[0] = 0x06f00513;
		code[1] = 0x00008067;
		publish(code, code + 2);
		if (((int (*)(void))code)() != 111)
			return -2;

		/* li a0, 222; ret */
		code[0] = 0x0de00513;
		publish(code, code + 2);
		if (((int (*)(void))code)() != 222)
			return -3;

		return 666;
	})M");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	machine.setup_linux_syscalls(false, false);
	machine.setup_linux({"program"}, {});
	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(machine.return_value<int>() == 666);
}

TEST_CASE("Guest JIT emits constants between functions", "[SMC]")
{
	// JITs put constant pools next to the code they belong to, which a linear
	// decode reads as instructions, losing the instruction boundary for
	// everything after it. Jumping into code behind a constant must still work.
	const auto binary = build_and_load(R"M(
	#include <sys/mman.h>
	extern long syscall(long, ...);

	int main() {
		unsigned int *code = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (code == MAP_FAILED)
			return -1;

		/* Entry: jump over the constant pool to the real code */
		code[0] = 0x0140006f;             /* j    +20            */
		/* Constants, decoding as instruction lengths 4, 4, 4, 2, 4, which
		   leaves the decode straddling the first real instruction */
		code[1] = 0x00000073;
		code[2] = 0x00000073;
		code[3] = 0x00000073;
		code[4] = 0x00734000;
		/* The real code, which a linear decode never lands on */
		code[5] = 0x1a400513;             /* li   a0, 420        */
		code[6] = 0x00008067;             /* ret                 */

		syscall(259 /* riscv_flush_icache */, code, code + 7, 0);

		return ((int (*)(void))code)();
	})M");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	machine.setup_linux_syscalls(false, false);
	machine.setup_linux({"program"}, {});
	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(machine.return_value<int>() == 420);
}

TEST_CASE("MAP_FIXED_NOREPLACE reserves an exact address", "[Mmap]")
{
	// Region-based allocators probe for a free region with MAP_FIXED_NOREPLACE
	// and assume that a success means they got the address they asked for.
	const auto binary = build_and_load(R"M(
	#include <errno.h>
	#include <sys/mman.h>

	#ifndef MAP_FIXED_NOREPLACE
	#define MAP_FIXED_NOREPLACE 0x100000
	#endif

	int main() {
		const unsigned long ADDR = 0x10000000000UL;
		const unsigned long SIZE = 0x100000UL;

		void *first = mmap((void *)ADDR, SIZE, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
		if (first != (void *)ADDR)
			return -1;
		*(volatile int *)first = 1234;
		if (*(volatile int *)first != 1234)
			return -2;

		/* The region is taken now, so the same request must fail */
		void *again = mmap((void *)ADDR, SIZE, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
		if (again != MAP_FAILED || errno != EEXIST)
			return -3;

		/* The next region up is free */
		void *second = mmap((void *)(ADDR * 2), SIZE, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
		if (second != (void *)(ADDR * 2))
			return -4;

		/* A kernel-chosen mapping must not come out of a reserved region */
		void *anon = mmap(0, SIZE, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (anon == MAP_FAILED)
			return -5;
		if ((unsigned long)anon + SIZE > ADDR && (unsigned long)anon < ADDR + SIZE)
			return -6;
		if (*(volatile int *)first != 1234)
			return -7;

		return 666;
	})M");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	machine.setup_linux_syscalls(false, false);
	machine.setup_linux({"program"}, {});
	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(machine.return_value<int>() == 666);
}

TEST_CASE("Zero-length iovec entries are accepted", "[Syscalls]")
{
	// musl writes to an unbuffered stream with iov[0] = {NULL, 0}, which must
	// not be range-checked as if it were a real buffer.
	const auto binary = build_and_load(R"M(
	#include <sys/uio.h>

	int main() {
		struct iovec iov[2] = {
			{ .iov_base = 0, .iov_len = 0 },
			{ .iov_base = "Hello World!", .iov_len = 12 },
		};
		if (writev(1, iov, 2) != 12)
			return -1;
		return 666;
	})M");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	machine.setup_linux_syscalls(false, false);
	machine.setup_linux({"program"}, {});
	machine.set_printer([] (const auto&, const char*, size_t) {});
	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(machine.return_value<int>() == 666);
}
