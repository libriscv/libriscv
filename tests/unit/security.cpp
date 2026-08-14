#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libriscv/machine.hpp>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2 -static", bool cpp = false);
extern std::vector<uint8_t> load_file(const std::string& filename);
static const std::string cwd {SRCDIR};
static const uint64_t MAX_MEMORY = 8ul << 20; /* 8MB */
static const uint64_t MAX_INSTRUCTIONS = 10'000'000ul;
using namespace riscv;

// =============================================================================
// C1: ioctl fallback passes guest register values as host pointers
//
// When has_file_descriptors() is true and the request code is not in the
// handled set (TCGETS, FIONBIO), guest-supplied values are passed directly
// to the host ioctl(). A guest can craft a request code that interprets
// its argument as a pointer, causing host memory read/write.
//
// The test verifies that ioctl with an unknown request code is denied
// when no filter is installed (it currently isn't — the fallback passes
// through, which is the bug).
// =============================================================================

TEST_CASE("C1: ioctl with unknown request should be denied", "[Security]")
{
	const auto binary = build_and_load(R"M(
	int main() {
		return 0;
	})M");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	machine.setup_linux_syscalls(true);
	machine.setup_linux(
		{"security_test"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	// Enable filesystem (which enables file descriptors)
	machine.fds().permit_filesystem = true;
	// Do NOT set filter_ioctl — this is the default config

	// Let the program run to completion first
	machine.simulate(MAX_INSTRUCTIONS);

	// Now test the ioctl handler directly by setting registers
	// as the guest would for ecall: a0=fd, a1=request, a2=arg
	// Syscall 29 = ioctl
	auto& cpu = machine.cpu;
	cpu.reg(riscv::REG_ARG0) = 1;          // fd = stdout (passes translate())
	cpu.reg(riscv::REG_ARG1) = 0x1234;     // unknown request code
	cpu.reg(riscv::REG_ARG2) = 0xDEAD0000; // would be treated as host pointer
	machine.system_call(29);

	// If the sandbox properly blocks unknown ioctls, result should be -ENOSYS or -EPERM.
	// If the fallback forwards to host, result will be -ENOTTY, -EINVAL, or -EFAULT
	// (host rejected because 0x1234 isn't valid, but it DID reach the host).
	const auto result = machine.return_value<int>();
	const bool was_denied = (result == -ENOSYS || result == -EPERM);
	// The test expects the sandbox to deny unknown ioctl requests.
	// If the request was forwarded to the host ioctl(), the bug exists.
	INFO("ioctl result: " << result << (was_denied ? " (denied)" : " (FORWARDED TO HOST)"));
	REQUIRE(was_denied);
}

// =============================================================================
// C2: Page-crossing reads in binary translation cache
//
// The fast-path memory accessors (rd16/rd32/rd64, wr16/wr32/wr64) in
// tr_api.cpp check only the page number, not whether the multi-byte access
// fits within the page. A read of 8 bytes at offset 0xFFC extends 4 bytes
// past the page buffer.
//
// This test verifies correctness of cross-page reads/writes through the
// interpreter path (which uses the slow path). The binary translation
// fast path has the bug but is not testable without RISCV_BINARY_TRANSLATION.
// =============================================================================

TEST_CASE("C2: Cross-page memory access correctness", "[Security]")
{
	const auto binary = build_and_load(R"M(
	#include <stdint.h>
	#include <string.h>
	#include <sys/mman.h>

	// Place a buffer that we know will cross a page boundary.
	// We'll write a known pattern at the page boundary and read it back.
	int main() {
		// Allocate memory that spans a page boundary
		char* base = (char*)mmap(NULL, 8192, PROT_READ | PROT_WRITE,
			MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		if (base == (void*)-1)
			return -1;

		// Find the last page boundary within our allocation
		uintptr_t addr = (uintptr_t)base;
		uintptr_t boundary = (addr + 4096) & ~(uintptr_t)0xFFF;

		// Write a uint64_t that straddles the page boundary
		// Place it at boundary - 4, so 4 bytes on each page
		char* cross = (char*)(boundary - 4);
		uint64_t write_val = 0xDEADBEEFCAFEBABEULL;
		memcpy(cross, &write_val, 8);

		// Read it back
		uint64_t read_val;
		memcpy(&read_val, cross, 8);

		if (read_val != write_val)
			return -2;

		// Also test 16-bit cross-page
		char* cross16 = (char*)(boundary - 1);
		uint16_t w16 = 0xABCD;
		memcpy(cross16, &w16, 2);
		uint16_t r16;
		memcpy(&r16, cross16, 2);
		if (r16 != w16)
			return -3;

		// Also test 32-bit cross-page
		char* cross32 = (char*)(boundary - 2);
		uint32_t w32 = 0x12345678;
		memcpy(cross32, &w32, 4);
		uint32_t r32;
		memcpy(&r32, cross32, 4);
		if (r32 != w32)
			return -4;

		return 42;
	})M");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	machine.setup_linux_syscalls();
	machine.setup_linux(
		{"security_test"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	machine.simulate(MAX_INSTRUCTIONS);
	REQUIRE(machine.return_value<int>() == 42);
}

// =============================================================================
// C3: faccessat bypasses all permission checks
//
// syscall_faccessat does not gate on has_file_descriptors() or check any
// filter. It hardcodes AT_FDCWD and passes guest-supplied paths straight
// to host faccessat(). Any guest can probe existence/permissions of
// arbitrary host files.
//
// The test verifies that faccessat is denied when file descriptors are
// not enabled, or when no filter is set.
// =============================================================================

TEST_CASE("C3: faccessat should respect permission checks", "[Security]")
{
	const auto binary = build_and_load(R"M(
	#include <unistd.h>
	#include <errno.h>

	// Use raw syscall to call faccessat (syscall 48 on RISC-V)
	long syscall(long n, ...);
	#define SYS_faccessat 48
	#define AT_FDCWD -100

	int main() {
		// Try to probe /etc/passwd — a file that exists on any Linux host
		long ret = syscall(SYS_faccessat, AT_FDCWD, "/etc/passwd", 0 /*F_OK*/, 0);
		if (ret == 0)
			return 1;  // Bug: guest could probe host filesystem
		if (ret == -1)
			return 42; // Correct: access was denied
		return (int)ret;
	})M");

	// Test 1: Machine WITHOUT file descriptors — faccessat should be denied
	{
		riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
		machine.setup_linux_syscalls(false);  // no filesystem
		machine.setup_linux(
			{"security_test"},
			{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

		machine.simulate(MAX_INSTRUCTIONS);
		// Guest should NOT be able to probe host files
		REQUIRE(machine.return_value<int>() == 42);
	}

	// Test 2: Machine WITH file descriptors but no filter — should also deny
	{
		riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
		machine.setup_linux_syscalls(true);
		machine.setup_linux(
			{"security_test"},
			{"LC_TYPE=C", "LC_ALL=C", "USER=root"});
		// permit_filesystem is false by default, no filter set
		// faccessat should still be denied

		machine.simulate(MAX_INSTRUCTIONS);
		REQUIRE(machine.return_value<int>() == 42);
	}
}

// =============================================================================
// C4: Missing tail guard page in the flat read-write arena
//
// The arena mapping is over-allocated by one page on each side. The fast-path
// accessors (Memory::read<T>/write<T>, used by guest LD/SD) bounds-check only
// the first byte of an access, so a multi-byte access at the last guest
// address (arena_size - 1) extends past the end of the guest address space.
// The tail guard page must absorb that overshoot, or the host takes an
// out-of-bounds read/write past its mapping.
// =============================================================================

TEST_CASE("C4: arena tail guard absorbs multi-byte access at last address", "[Security]")
{
	const auto binary = load_file(cwd + "/elf/newlib-rv64gb-hello-world");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };

	const auto arena_size = machine.memory.memory_arena_size();
	REQUIRE(arena_size > Page::size());

	// Locate the host pointer for guest address 0x1000
	auto v = machine.memory.memview(0x1000, 16);
	REQUIRE(v.size() == 16);
	const uint8_t* arena_data = (const uint8_t*)v.data() - 0x1000;

	// The 8 bytes at the end of the guest address space must be backed by
	// host memory. Probing with write() turns a missing tail guard into
	// EFAULT instead of a crash.
	const int fd = open("/dev/null", O_WRONLY);
	REQUIRE(fd >= 0);
	const ssize_t probe = write(fd, arena_data + arena_size - 8, 8);
	close(fd);
	REQUIRE(probe == 8);

	// The fast path must survive an 8-byte access at the last guest
	// address (the overshoot lands in the tail guard page)
	REQUIRE_NOTHROW(machine.memory.read<uint64_t>(arena_size - 1));
	REQUIRE_NOTHROW(machine.memory.write<uint64_t>(arena_size - 1, 0x4141414141414141u));

	// The bulk APIs check addr+len and must still reject the same access
	REQUIRE_THROWS_AS(machine.memory.memview(arena_size - 1, 8), riscv::MachineException);
#ifndef RISCV_VIRTUAL_PAGING
	// Without paging there is no on-demand page table: an aligned access
	// starting at the arena end is fully outside guest memory and must fault
	REQUIRE_THROWS_AS(machine.memory.read<uint64_t>(arena_size), riscv::MachineException);
#endif
}

// =============================================================================
// C5: Unbounded nanosleep blocks the host thread
//
// syscall 101 (nanosleep) passed the guest-controlled tv_sec straight to the
// host nanosleep(), so a guest could block the host thread for years while
// the instruction counter stood still, violating the termination guarantee.
// The duration must be clamped and the instruction counter penalized in
// proportion to the requested time.
// =============================================================================

TEST_CASE("C5: nanosleep is clamped and penalized", "[Security]")
{
	const auto binary = load_file(cwd + "/elf/newlib-rv64gb-hello-world");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	machine.setup_linux_syscalls(false, false);

	const auto ts_addr = machine.memory.mmap_allocate(4096);
	REQUIRE(ts_addr != 0);

	// Request a 2-second sleep; it must be clamped to ~1 second
	struct kernel_timespec { int64_t tv_sec; int64_t tv_nsec; };
	const kernel_timespec ts { .tv_sec = 2, .tv_nsec = 0 };
	machine.memory.memcpy(ts_addr, &ts, sizeof(ts));

	auto& cpu = machine.cpu;
	cpu.reg(riscv::REG_ARG0) = ts_addr;
	cpu.reg(riscv::REG_ARG1) = 0x0; // rem = NULL

	const auto t0 = std::chrono::steady_clock::now();
	machine.system_call(101); // nanosleep
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - t0).count();

	REQUIRE(machine.return_value<long>() == 0);
	// Clamped: strictly less than the requested 2 seconds
	REQUIRE(elapsed < 1900);
	// Penalized: 2 seconds requested = 2,000,000 instructions
	REQUIRE(machine.instruction_counter() >= 2'000'000);

	// Negative durations must be rejected without sleeping
	const kernel_timespec bad { .tv_sec = 0, .tv_nsec = -1 };
	machine.memory.memcpy(ts_addr, &bad, sizeof(bad));
	cpu.reg(riscv::REG_ARG0) = ts_addr;
	cpu.reg(riscv::REG_ARG1) = 0x0;
	machine.system_call(101);
	REQUIRE(machine.return_value<int>() == -22); // -EINVAL
}

// =============================================================================
// C6: getcwd throws a machine exception when file descriptors are disabled
//
// syscall_getcwd dereferenced machine.fds() unconditionally. With
// setup_linux_syscalls(false, false) a guest calling getcwd() aborted the
// simulation with a C++ exception instead of an error return.
// =============================================================================

TEST_CASE("C6: getcwd returns -EBADF without file descriptors", "[Security]")
{
	const auto binary = load_file(cwd + "/elf/newlib-rv64gb-hello-world");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	machine.setup_linux_syscalls(false, false);

	const auto buf = machine.memory.mmap_allocate(4096);
	auto& cpu = machine.cpu;
	cpu.reg(riscv::REG_ARG0) = buf;
	cpu.reg(riscv::REG_ARG1) = 4096;

	REQUIRE_NOTHROW(machine.system_call(17)); // getcwd
	REQUIRE(machine.return_value<int>() == -9); // -EBADF
}

// =============================================================================
// C7: pselect throws a machine exception when file descriptors are disabled
//
// syscall_pselect threw SYSTEM_CALL_FAILED unconditionally, letting a guest
// abort the simulation with a single call.
// =============================================================================

TEST_CASE("C7: pselect returns 0 without file descriptors", "[Security]")
{
	const auto binary = load_file(cwd + "/elf/newlib-rv64gb-hello-world");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	machine.setup_linux_syscalls(false, false);

	REQUIRE_NOTHROW(machine.system_call(72)); // pselect6
	REQUIRE(machine.return_value<int>() == 0);
}


