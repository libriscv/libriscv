#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libriscv/machine.hpp>
extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2 -static", bool cpp = false);
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


