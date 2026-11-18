#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libriscv/machine.hpp>
#include <libriscv/threads.hpp>
#include <chrono>
#include <functional>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
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



// =============================================================================
// C8: tgkill on a suspended or blocked thread leaves a dangling pointer
//
// Thread::exit() -> erase_thread() removed the thread from m_threads only.
// Killing a thread that is not running (tgkill with an unhandled signal)
// destroyed it while m_suspended or m_blocked still pointed at it. The next
// sched_yield (resume) or FUTEX_WAKE (wakeup_blocked) then used it after free.
// =============================================================================

static int setup_two_threads(riscv::Machine<RISCV64>& machine, uint64_t stack)
{
	auto& cpu = machine.cpu;
	// clone(flags=0, stack, ...): main is suspended, the child is running
	cpu.reg(riscv::REG_ARG0) = 0;
	cpu.reg(riscv::REG_ARG1) = stack;
	cpu.reg(riscv::REG_ARG4) = 0;
	cpu.reg(riscv::REG_ARG5) = 0;
	cpu.reg(riscv::REG_ARG6) = 0;
	machine.system_call(220);
	REQUIRE(machine.threads().get_tid() != riscv::MAIN_THREAD_TID);
	REQUIRE(machine.threads().suspended_threads().size() == 1);
	return machine.threads().get_tid();
}

static void tgkill(riscv::Machine<RISCV64>& machine, int tid, int sig)
{
	auto& cpu = machine.cpu;
	cpu.reg(riscv::REG_ARG0) = riscv::MAIN_THREAD_TID;
	cpu.reg(riscv::REG_ARG1) = tid;
	cpu.reg(riscv::REG_ARG2) = sig;
	machine.system_call(131);
}

TEST_CASE("C8: tgkill on a suspended thread removes it from the scheduler", "[Security]")
{
	const auto binary = load_file(cwd + "/elf/newlib-rv64gb-hello-world");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	machine.setup_linux_syscalls(false, false);
	machine.setup_posix_threads();
	const auto stack = machine.memory.mmap_allocate(65536) + 65536;
	auto& threads = machine.threads();

	REQUIRE(setup_two_threads(machine, stack) == 2);
	// The child yields back to main, and is now suspended
	machine.system_call(124); // sched_yield
	REQUIRE(threads.get_tid() == riscv::MAIN_THREAD_TID);
	REQUIRE(threads.suspended_threads().size() == 1);

	// Main kills the suspended child (SIGTERM has no handler)
	tgkill(machine, 2, 15);
	REQUIRE(threads.get_thread(2) == nullptr);
	REQUIRE(threads.suspended_threads().empty());
	REQUIRE(threads.get_tid() == riscv::MAIN_THREAD_TID);

	// Yielding must not resume the destroyed thread
	machine.system_call(124); // sched_yield
	REQUIRE(threads.get_tid() == riscv::MAIN_THREAD_TID);
	REQUIRE(machine.return_value<int>() == 0);

	// A fork must not copy the destroyed thread either
	riscv::MultiThreading<RISCV64> copy { machine, threads };
	REQUIRE(copy.suspended_threads().empty());
	REQUIRE(copy.blocked_threads().empty());
}

TEST_CASE("C8: tgkill on a blocked thread removes it from the scheduler", "[Security]")
{
	const auto binary = load_file(cwd + "/elf/newlib-rv64gb-hello-world");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	machine.setup_linux_syscalls(false, false);
	machine.setup_posix_threads();
	const auto stack = machine.memory.mmap_allocate(65536) + 65536;
	const auto futex_word = machine.memory.mmap_allocate(4096);
	machine.memory.write<uint32_t>(futex_word, 0);
	auto& threads = machine.threads();
	auto& cpu = machine.cpu;

	REQUIRE(setup_two_threads(machine, stack) == 2);
	// The child waits on the futex, blocks, and main resumes
	cpu.reg(riscv::REG_ARG0) = futex_word;
	cpu.reg(riscv::REG_ARG1) = 0; // FUTEX_WAIT
	cpu.reg(riscv::REG_ARG2) = 0; // expected value
	cpu.reg(riscv::REG_ARG5) = 0;
	machine.system_call(98);
	REQUIRE(threads.get_tid() == riscv::MAIN_THREAD_TID);
	REQUIRE(threads.blocked_threads().size() == 1);
	REQUIRE(threads.suspended_threads().empty());

	// Main kills the blocked child
	tgkill(machine, 2, 15);
	REQUIRE(threads.get_thread(2) == nullptr);
	REQUIRE(threads.blocked_threads().empty());

	// Waking the futex must not touch the destroyed thread
	cpu.reg(riscv::REG_ARG0) = futex_word;
	cpu.reg(riscv::REG_ARG1) = 1; // FUTEX_WAKE
	cpu.reg(riscv::REG_ARG2) = 1;
	cpu.reg(riscv::REG_ARG5) = 0;
	machine.system_call(98);
	REQUIRE(machine.return_value<int>() == 0);
	REQUIRE(threads.suspended_threads().empty());
	REQUIRE(threads.get_tid() == riscv::MAIN_THREAD_TID);
}

// =============================================================================
// C9: the main thread stays in m_blocked when the fallback resumes it
//
// When the running thread exited with nothing suspended, wakeup_next() fell
// back to resuming the main thread, even though it was blocked, and left it in
// m_blocked. Each round of (clone; the child yields; main waits on a futex;
// the child exits) added another entry, and every exit and FUTEX_WAKE scans
// the list: host time grew with the square of guest instructions. A FUTEX_WAKE
// could also queue main while it was running.
// =============================================================================

TEST_CASE("C9: a resumed thread leaves the blocked list", "[Security]")
{
	const auto binary = load_file(cwd + "/elf/newlib-rv64gb-hello-world");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	machine.setup_linux_syscalls(false, false);
	machine.setup_posix_threads();
	const auto stack = machine.memory.mmap_allocate(65536) + 65536;
	const auto futex_word = machine.memory.mmap_allocate(4096);
	machine.memory.write<uint32_t>(futex_word, 0);
	auto& threads = machine.threads();
	auto& cpu = machine.cpu;

	for (int round = 0; round < 10; round++)
	{
		setup_two_threads(machine, stack);
		// The child yields back to main
		machine.system_call(124); // sched_yield
		REQUIRE(threads.get_tid() == riscv::MAIN_THREAD_TID);
		// Main waits on the futex, and the child runs
		cpu.reg(riscv::REG_ARG0) = futex_word;
		cpu.reg(riscv::REG_ARG1) = 0; // FUTEX_WAIT
		cpu.reg(riscv::REG_ARG2) = 0;
		cpu.reg(riscv::REG_ARG5) = 0;
		machine.system_call(98);
		REQUIRE(threads.get_tid() != riscv::MAIN_THREAD_TID);
		REQUIRE(threads.blocked_threads().size() == 1);
		// The child exits with nothing suspended: fallback to main
		cpu.reg(riscv::REG_ARG0) = 0;
		machine.system_call(93); // exit
		REQUIRE(threads.get_tid() == riscv::MAIN_THREAD_TID);
		REQUIRE(threads.blocked_threads().empty());
		REQUIRE(threads.suspended_threads().empty());
	}

	// Waking the futex must not queue the running main thread
	cpu.reg(riscv::REG_ARG0) = futex_word;
	cpu.reg(riscv::REG_ARG1) = 1; // FUTEX_WAKE
	cpu.reg(riscv::REG_ARG2) = 1;
	cpu.reg(riscv::REG_ARG5) = 0;
	machine.system_call(98);
	REQUIRE(machine.return_value<int>() == 0);
	REQUIRE(threads.suspended_threads().empty());
}

TEST_CASE("C9: queueing an already queued thread is an error", "[Security]")
{
	const auto binary = load_file(cwd + "/elf/newlib-rv64gb-hello-world");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	machine.setup_linux_syscalls(false, false);
	machine.setup_posix_threads();
	auto* main_thread = machine.threads().get_thread();

	main_thread->suspend();
	REQUIRE(machine.threads().suspended_threads().size() == 1);
	REQUIRE_THROWS_AS(main_thread->suspend(), riscv::MachineException);
	REQUIRE_THROWS_AS(main_thread->block(0), riscv::MachineException);
	REQUIRE(machine.threads().suspended_threads().size() == 1);
	REQUIRE(machine.threads().blocked_threads().empty());
}

// =============================================================================
// C10: per-thread signal state leaks
//
// Signals::per_thread() created an entry (altstack and a full register file)
// on first use, including for a read-only sigaltstack(NULL, &old) and for
// rt_sigreturn, and nothing erased it when the thread exited. TIDs are not
// reused, so creating threads in a loop grew host memory outside memory_max.
// =============================================================================

static void sigaltstack(riscv::Machine<RISCV64>& machine, uint64_t ss, uint64_t old_ss)
{
	machine.cpu.reg(riscv::REG_ARG0) = ss;
	machine.cpu.reg(riscv::REG_ARG1) = old_ss;
	machine.system_call(132);
	REQUIRE(machine.return_value<int>() == 0);
}

TEST_CASE("C10: reading signal state does not create it", "[Security]")
{
	const auto binary = load_file(cwd + "/elf/newlib-rv64gb-hello-world");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	machine.setup_linux_syscalls(false, false);
	machine.setup_posix_threads();
	const auto area = machine.memory.mmap_allocate(65536);

	// sigaltstack(NULL, &old) reads a zeroed stack (padding included)
	std::array<uint8_t, sizeof(riscv::SignalStack<RISCV64>)> old_ss;
	machine.memory.memset(area, 0xAA, old_ss.size());
	sigaltstack(machine, 0, area);
	machine.memory.memcpy_out(old_ss.data(), area, old_ss.size());
	for (auto byte : old_ss)
		REQUIRE(byte == 0);
	REQUIRE(machine.signals().per_thread_count() == 0);

	// rt_sigreturn without a delivered signal
	machine.cpu.reg(riscv::REG_SP) = area + 4096;
	machine.system_call(139);
	REQUIRE(machine.signals().per_thread_count() == 0);

	// Setting an altstack creates it, and reading it back sees it
	const riscv::SignalStack<RISCV64> new_ss { .ss_sp = area, .ss_flags = 0, .ss_size = 8192 };
	machine.copy_to_guest(area + 128, &new_ss, sizeof(new_ss));
	sigaltstack(machine, area + 128, 0);
	REQUIRE(machine.signals().per_thread_count() == 1);
	sigaltstack(machine, 0, area + 256);
	REQUIRE(machine.memory.read<uint64_t>(area + 256) == area);
	REQUIRE(machine.memory.read<uint64_t>(area + 256 + 16) == 8192);
}

TEST_CASE("C10: an exited thread's signal state is erased", "[Security]")
{
	const auto binary = load_file(cwd + "/elf/newlib-rv64gb-hello-world");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	machine.setup_linux_syscalls(false, false);
	machine.setup_posix_threads();
	const auto stack = machine.memory.mmap_allocate(65536) + 65536;
	const auto area = machine.memory.mmap_allocate(4096);
	const riscv::SignalStack<RISCV64> new_ss { .ss_sp = area, .ss_flags = 0, .ss_size = 2048 };
	machine.copy_to_guest(area, &new_ss, sizeof(new_ss));
	auto& threads = machine.threads();

	for (int round = 0; round < 100; round++)
	{
		const int tid = setup_two_threads(machine, stack);
		// The child sets an altstack (which creates its signal state)
		sigaltstack(machine, area, 0);
		REQUIRE(machine.signals().find_per_thread(tid) != nullptr);
		if (round % 2 == 0) {
			// ... and exits
			machine.cpu.reg(riscv::REG_ARG0) = 0;
			machine.system_call(93); // exit
		} else {
			// ... or yields, and main kills it
			machine.system_call(124); // sched_yield
			REQUIRE(threads.get_tid() == riscv::MAIN_THREAD_TID);
			tgkill(machine, tid, 15);
		}
		REQUIRE(threads.get_tid() == riscv::MAIN_THREAD_TID);
		REQUIRE(threads.get_thread(tid) == nullptr);
		REQUIRE(machine.signals().find_per_thread(tid) == nullptr);
	}
	REQUIRE(machine.signals().per_thread_count() == 0);
}

static constexpr uint32_t WFI = 0x10500073;
static constexpr riscv::PageAttributes RWX {.read = true, .write = true, .exec = true};

// Every decoded byte is charged (len / 4), so leave room for decoding megabytes
static void execute_at(riscv::Machine<RISCV64>& machine, uint64_t pc, uint64_t max = 10'000'000)
{
	machine.memory.write<uint32_t>(pc, WFI);
	machine.cpu.jump(pc);
	machine.simulate<false>(max);
}

static riscv::Machine<RISCV64> machine_with_main_segment(riscv::MachineOptions<RISCV64> options)
{
	static const std::vector<uint8_t> empty;
	riscv::Machine<RISCV64> machine { empty, options };
	machine.memory.set_page_attr(0x1000, 0x1000, RWX);
	execute_at(machine, 0x1000);
	REQUIRE(machine.memory.execute_segments_count() == 0);
	return machine;
}

TEST_CASE("C11: overlapping execute segments do not accumulate", "[Security]")
{
	static constexpr uint64_t AREA = 0x100000;
	static constexpr uint64_t AREA_LEN = 2ul << 20;
	auto machine = machine_with_main_segment({ .memory_max = 64ul << 20 });
	const uint64_t main = machine.memory.execute_segments_memory();

	machine.memory.set_page_attr(AREA, AREA_LEN, RWX);
	execute_at(machine, AREA);
	const uint64_t one_copy = machine.memory.execute_segments_memory() - main;
	REQUIRE(one_copy > 2 * AREA_LEN);
	REQUIRE(one_copy < 10 * AREA_LEN);

	uint64_t end = AREA + AREA_LEN;
	for (size_t i = 1; i < RISCV_MAX_EXECUTE_SEGS; i++)
	{
		machine.memory.set_page_attr(end, 0x1000, RWX);
		execute_at(machine, end);
		end += 0x1000;
		REQUIRE(!machine.instruction_limit_reached());
		REQUIRE(machine.memory.execute_segments_count() == i + 1);
		REQUIRE(machine.memory.execute_segments_memory() < main + one_copy + one_copy / 16);
		auto& segment = machine.memory.exec_segment_for(AREA);
		REQUIRE(segment->exec_begin() == AREA);
		REQUIRE(segment->exec_end() == end);
	}
	// Evicted slots stay taken
	machine.memory.set_page_attr(end, 0x1000, RWX);
	REQUIRE_THROWS_WITH(execute_at(machine, end),
		Catch::Matchers::ContainsSubstring("Max execute segments"));
}

TEST_CASE("C11: execute segments are bounded by their memory limit", "[Security]")
{
	static constexpr uint64_t LEN = 2ul << 20;
	auto machine = machine_with_main_segment({
		.memory_max = 128ul << 20,
		.execute_segment_memory_max = 32ul << 20,
	});
	REQUIRE(machine.memory.execute_segments_memory_max() == (32ul << 20));

	size_t created = 0;
	for (; created < RISCV_MAX_EXECUTE_SEGS; created++)
	{
		const uint64_t addr = 0x100000 + 2 * LEN * created;
		machine.memory.set_page_attr(addr, LEN, RWX);
		const uint64_t before = machine.memory.execute_segments_memory();
		try {
			execute_at(machine, addr);
		} catch (const riscv::MachineException& e) {
			REQUIRE(e.type() == riscv::OUT_OF_MEMORY);
			REQUIRE(machine.memory.execute_segments_memory() == before);
			REQUIRE(machine.memory.execute_segments_count() == created);
			break;
		}
		REQUIRE(machine.memory.execute_segments_memory() <= (32ul << 20));
	}
	REQUIRE(created >= 1);
	REQUIRE(created < RISCV_MAX_EXECUTE_SEGS);

	// The default is max(memory_max, 256MB)
	auto other = machine_with_main_segment({ .memory_max = 64ul << 20 });
	REQUIRE(other.memory.execute_segments_memory_max() == (256ul << 20));
}

TEST_CASE("C11: decoding code is charged", "[Security]")
{
	static constexpr uint64_t AREA = 0x100000;
	static constexpr uint64_t AREA_LEN = 1ul << 20;

	// Each decode costs len / 4
	auto charged = machine_with_main_segment({ .memory_max = 64ul << 20 });
	charged.memory.set_page_attr(AREA, AREA_LEN, RWX);
	execute_at(charged, AREA, 10'000'000);
	REQUIRE(charged.instruction_counter() >= AREA_LEN / 4);
	REQUIRE(charged.instruction_counter() < AREA_LEN / 4 + 10);
	REQUIRE(!charged.instruction_limit_reached());

	// ... and the limit is checked before the new segment runs
	auto limited = machine_with_main_segment({ .memory_max = 64ul << 20 });
	limited.memory.set_page_attr(AREA, AREA_LEN, RWX);
	execute_at(limited, AREA, 1000);
	REQUIRE(limited.instruction_limit_reached());
	REQUIRE(limited.cpu.pc() == AREA);
}

TEST_CASE("C11: rebuilding code after FENCE.I is charged", "[Security]")
{
	static constexpr uint64_t AREA = 0x100000;
	static constexpr uint64_t AREA_LEN = 64ul << 10;
	auto machine = machine_with_main_segment({ .memory_max = 64ul << 20 });
	machine.memory.set_page_attr(AREA, AREA_LEN, RWX);
	machine.memory.write<uint32_t>(AREA + 0, 0x0000100F); // fence.i
	machine.memory.write<uint32_t>(AREA + 4, 0x00130313); // addi t1, t1, 1
	machine.memory.write<uint32_t>(AREA + 8, 0xFF9FF06F); // j AREA
	machine.cpu.jump(AREA);
	machine.simulate<false>(1'000'000);

	REQUIRE(machine.instruction_limit_reached());
	REQUIRE(machine.cpu.reg(6) > 0);
	REQUIRE(machine.cpu.reg(6) <= 1'000'000 / (AREA_LEN / 4) + 1);
}

TEST_CASE("C11: a preempted segment is not evicted", "[Security]")
{
	static constexpr uint64_t AREA = 0x100000;
	auto machine = machine_with_main_segment({ .memory_max = 64ul << 20 });
	machine.memory.set_page_attr(AREA, 0x2000, RWX);
	machine.memory.write<uint32_t>(AREA + 0, 0x1F400893); // li a7, 500
	machine.memory.write<uint32_t>(AREA + 4, 0x00000073); // ecall
	machine.memory.write<uint32_t>(AREA + 8, WFI);
	machine.memory.set_page_attr(AREA + 0x1000, 0x1000, {.read = true, .write = true});

	static bool preempted;
	preempted = false;
	machine.install_syscall_handler(500, [] (auto& m) {
		// Extend the area the outer call is executing, and call into it
		m.memory.set_page_attr(AREA + 0x1000, 0x1000, RWX);
		m.memory.template write<uint32_t>(AREA + 0x1000, WFI);
		m.preempt(100'000, AREA + 0x1000);
		REQUIRE(m.memory.execute_segments_count() == 2);
		REQUIRE(m.memory.exec_segment_for(AREA)->exec_end() == AREA + 0x1000);
		preempted = true;
	});
	machine.cpu.jump(AREA);
	machine.simulate(100'000);
	REQUIRE(preempted);
	REQUIRE(machine.cpu.pc() == AREA + 12);

	// Back at the top level, the next segment evicts both
	machine.memory.set_page_attr(AREA + 0x2000, 0x1000, RWX);
	execute_at(machine, AREA + 0x2000);
	REQUIRE(machine.memory.execute_segments_count() == 3);
	REQUIRE(machine.memory.exec_segment_for(AREA)->exec_end() == AREA + 0x3000);
	REQUIRE(machine.memory.exec_segment_for(AREA + 0x1000).get()
		== machine.memory.exec_segment_for(AREA).get());
}

template <int W>
static void discard_past_the_top(riscv::address_type<W> dst, size_t len, riscv::address_type<W> far)
{
	static const std::vector<uint8_t> empty;
	riscv::Machine<W> machine { empty, { .memory_max = MAX_MEMORY } };
	if constexpr (!riscv::virtual_paging_enabled) {
		REQUIRE_THROWS(machine.memory.memdiscard(dst, len, true));
		return;
	}
	machine.memory.template write<uint64_t>(0x2000, 1);
	machine.memory.template write<uint64_t>(0x8000, 2);
	machine.memory.template write<uint64_t>(far, 3);
	machine.memory.memdiscard(dst, len, true);
	REQUIRE(machine.memory.template read<uint64_t>(0x2000) == uint64_t(dst <= 0x2000 ? 0 : 1));
	REQUIRE(machine.memory.template read<uint64_t>(0x8000) == 0);
	REQUIRE(machine.memory.template read<uint64_t>(far) == 0);
}

TEST_CASE("C12: discarding past the top of the address space ends there", "[Security]")
{
	static constexpr uint64_t FAR64 = 0x1000000000;
	for (const size_t len : { ~size_t(0), size_t(0xFFFFFFFFFFFFF930), size_t(-0x3000) })
		discard_past_the_top<RISCV64>(0x3000, len, FAR64);
	discard_past_the_top<RISCV64>(0x3800, ~size_t(0), FAR64);
	discard_past_the_top<RISCV64>(0x0, ~size_t(0), FAR64);

	static constexpr uint32_t FAR32 = 0x80000000;
	for (const size_t len : { ~size_t(0), size_t(0xFFFFF930), size_t(0x100000000 - 0x3000), size_t(1) << 40 })
		discard_past_the_top<RISCV32>(0x3000, len, FAR32);
	discard_past_the_top<RISCV32>(0x3800, ~size_t(0), FAR32);
	discard_past_the_top<RISCV32>(0x0, ~size_t(0), FAR32);
}

TEST_CASE("C12: madvise(MADV_DONTNEED) with a wrapping length", "[Security]")
{
	static const std::vector<uint8_t> empty;
	Machine<RISCV64> machine { empty, { .memory_max = MAX_MEMORY } };
	machine.setup_linux_syscalls(false, false);
	machine.cpu.reg(riscv::REG_ARG0) = 0x203000;
	machine.cpu.reg(riscv::REG_ARG1) = ~uint64_t(0);
	machine.cpu.reg(riscv::REG_ARG2) = 4;
	if constexpr (!riscv::virtual_paging_enabled) {
		REQUIRE_THROWS(machine.system_call(233));
		return;
	}
	machine.memory.write<uint64_t>(0x203000, 1);
	machine.system_call(233);
	REQUIRE(machine.return_value<int>() == 0);
	REQUIRE(machine.memory.read<uint64_t>(0x203000) == 0);
}

TEST_CASE("C13: guest paths are normalised", "[Security]")
{
	auto n = riscv::FileDescriptors::normalize_path;
	REQUIRE(n("/etc/hosts") == "/etc/hosts");
	REQUIRE(n("//etc///./hosts") == "/etc/hosts");
	REQUIRE(n("/srv/data/../../etc/shadow") == "/etc/shadow");
	REQUIRE(n("/../../..") == "/");
	REQUIRE(n("/a/b/..") == "/a/");
	REQUIRE(n("/a/b/.") == "/a/b/");
	REQUIRE(n("/a/b/") == "/a/b/");
	REQUIRE(n("/") == "/");
	REQUIRE(n("") == "/");
	REQUIRE(n("a/./b") == "/a/b");
	REQUIRE(n("/a/..b/.c") == "/a/..b/.c");
}

namespace {
struct SandboxDir {
	std::string dir;
	SandboxDir() {
		char tmpl[] = "/tmp/libriscv_sandbox_XXXXXX";
		REQUIRE(mkdtemp(tmpl) != nullptr);
		dir = tmpl;
		const int fd = ::open((dir + "/data").c_str(), O_WRONLY | O_CREAT, 0644);
		REQUIRE(fd >= 0);
		REQUIRE(::write(fd, "hello", 5) == 5);
		::close(fd);
		REQUIRE(mkfifo((dir + "/fifo").c_str(), 0644) == 0);
		REQUIRE(symlink((dir + "/fifo").c_str(), (dir + "/link_fifo").c_str()) == 0);
		REQUIRE(symlink((dir + "/data").c_str(), (dir + "/link_data").c_str()) == 0);
	}
	~SandboxDir() {
		for (const char* name : {"/data", "/fifo", "/link_fifo", "/link_data", "/created"})
			::unlink((dir + name).c_str());
		::rmdir(dir.c_str());
	}
	std::string operator/ (const char* name) const { return dir + "/" + name; }
};

struct SandboxMachine {
	riscv::Machine<RISCV64> machine { std::vector<uint8_t>{}, { .memory_max = MAX_MEMORY } };
	uint64_t scratch = 0;
	std::vector<std::string> seen;

	SandboxMachine() {
		machine.setup_linux_syscalls(true, false);
		machine.fds().permit_filesystem = true;
		scratch = machine.memory.mmap_allocate(8192);
	}
	void prefix_filter(const std::string& prefix) {
		machine.fds().filter_open = [this, prefix] (void*, std::string& path) {
			seen.push_back(path);
			return path.rfind(prefix, 0) == 0;
		};
	}
	long syscall(int n, int fd, const std::string& path, uint64_t a2 = 0, uint64_t a3 = 0) {
		machine.copy_to_guest(scratch, path.c_str(), path.size() + 1);
		machine.cpu.reg(riscv::REG_ARG0) = fd;
		machine.cpu.reg(riscv::REG_ARG1) = scratch;
		machine.cpu.reg(riscv::REG_ARG2) = a2;
		machine.cpu.reg(riscv::REG_ARG3) = a3;
		machine.system_call(n);
		return machine.return_value<long>();
	}
	long openat(int fd, const std::string& path, int flags = 0) { return syscall(56, fd, path, flags, 0644); }
	long faccessat(int fd, const std::string& path) { return syscall(48, fd, path, 0, 0); }
	long fstatat(int fd, const std::string& path) { return syscall(79, fd, path, scratch + 4096, 0); }
	long readlinkat(int fd, const std::string& path) { return syscall(78, fd, path, scratch + 4096, 256); }
};
static constexpr int GUEST_AT_FDCWD = -100;
}

TEST_CASE("C13: without a filter, paths reach the host only in proxy mode", "[Security]")
{
	SandboxDir dir;
	SandboxMachine m;
	REQUIRE(m.openat(GUEST_AT_FDCWD, dir / "data") == -EPERM);
	REQUIRE(m.faccessat(GUEST_AT_FDCWD, dir / "data") == -EACCES);
	REQUIRE(m.fstatat(GUEST_AT_FDCWD, dir / "data") == -EPERM);
	REQUIRE(m.readlinkat(GUEST_AT_FDCWD, dir / "link_data") == -EPERM);

	m.machine.fds().proxy_mode = true;
	REQUIRE(m.openat(GUEST_AT_FDCWD, dir / "data") >= riscv::FileDescriptors::FILE_D_BASE);
	REQUIRE(m.faccessat(GUEST_AT_FDCWD, dir / "data") == 0);
	REQUIRE(m.fstatat(GUEST_AT_FDCWD, dir / "data") == 0);
	REQUIRE(m.readlinkat(GUEST_AT_FDCWD, dir / "link_data") == long((dir / "data").size()));
}

TEST_CASE("C13: filters see normalised absolute paths", "[Security]")
{
	SandboxDir dir;
	SandboxMachine m;
	m.prefix_filter(dir.dir + "/");
	const std::string escape = dir / "../../etc/passwd";

	REQUIRE(m.openat(GUEST_AT_FDCWD, escape) == -EPERM);
	REQUIRE(m.seen.back() == "/etc/passwd");
	REQUIRE(m.faccessat(GUEST_AT_FDCWD, escape) == -EACCES);
	REQUIRE(m.seen.back() == "/etc/passwd");
	REQUIRE(m.fstatat(GUEST_AT_FDCWD, escape) == -EPERM);
	REQUIRE(m.seen.back() == "/etc/passwd");

	REQUIRE(m.openat(GUEST_AT_FDCWD, "data") == -EBADF);
	REQUIRE(m.openat(GUEST_AT_FDCWD, "") == -ENOENT);

	const long dirfd = m.openat(GUEST_AT_FDCWD, dir.dir + "//./");
	REQUIRE(dirfd >= riscv::FileDescriptors::FILE_D_BASE);
	REQUIRE(m.seen.back() == dir.dir + "/");
	REQUIRE(m.openat(dirfd, "../../etc/passwd") == -EPERM);
	REQUIRE(m.seen.back() == "/etc/passwd");
	REQUIRE(m.fstatat(dirfd, "../../etc/passwd") == -EPERM);
	REQUIRE(m.faccessat(dirfd, "data") == 0);
	REQUIRE(m.seen.back() == dir / "data");

	const long fd = m.openat(dirfd, "./data");
	REQUIRE(fd >= riscv::FileDescriptors::FILE_D_BASE);
	REQUIRE(m.seen.back() == dir / "data");
	REQUIRE(m.machine.fds().guest_paths.at(fd) == dir / "data");
	m.machine.cpu.reg(riscv::REG_ARG0) = fd;
	m.machine.system_call(57);
	REQUIRE(m.machine.fds().guest_paths.count(fd) == 0);
}

TEST_CASE("C13: write access needs proxy mode", "[Security]")
{
	static constexpr int GUEST_O_WRONLY = 01, GUEST_O_RDWR = 02, GUEST_O_CREAT = 0100,
		GUEST_O_TRUNC = 01000, GUEST_O_APPEND = 02000;
	SandboxDir dir;
	SandboxMachine m;
	m.prefix_filter(dir.dir + "/");

	REQUIRE(m.openat(GUEST_AT_FDCWD, dir / "data", GUEST_O_TRUNC) == -EACCES);
	REQUIRE(m.openat(GUEST_AT_FDCWD, dir / "data", GUEST_O_WRONLY) == -EACCES);
	REQUIRE(m.openat(GUEST_AT_FDCWD, dir / "data", GUEST_O_RDWR) == -EACCES);
	REQUIRE(m.openat(GUEST_AT_FDCWD, dir / "data", GUEST_O_APPEND) == -EACCES);
	REQUIRE(m.openat(GUEST_AT_FDCWD, dir / "created", GUEST_O_CREAT) == -EACCES);
	struct stat st;
	REQUIRE(::stat((dir / "data").c_str(), &st) == 0);
	REQUIRE(st.st_size == 5);
	REQUIRE(::stat((dir / "created").c_str(), &st) != 0);

	m.machine.fds().proxy_mode = true;
	REQUIRE(m.openat(GUEST_AT_FDCWD, dir / "created", GUEST_O_WRONLY | GUEST_O_CREAT) >= 0);
	REQUIRE(::stat((dir / "created").c_str(), &st) == 0);
}

TEST_CASE("C13: special files are refused unless permitted", "[Security]")
{
	SandboxDir dir;
	SandboxMachine m;
	m.machine.fds().filter_open = [] (void*, std::string&) { return true; };

	REQUIRE(m.openat(GUEST_AT_FDCWD, dir / "fifo") == -EACCES);
	REQUIRE(m.openat(GUEST_AT_FDCWD, dir / "link_fifo") == -EACCES);
	REQUIRE(m.openat(GUEST_AT_FDCWD, "/dev/null") == -EACCES);

	const long fd = m.openat(GUEST_AT_FDCWD, dir / "link_data");
	REQUIRE(fd >= riscv::FileDescriptors::FILE_D_BASE);
	const int real_fd = m.machine.fds().translate(fd);
	REQUIRE((::fcntl(real_fd, F_GETFL) & O_NONBLOCK) == 0);
	char buf[8] = {};
	REQUIRE(::read(real_fd, buf, sizeof(buf)) == 5);

	m.machine.fds().permit_special_files = true;
	REQUIRE(m.openat(GUEST_AT_FDCWD, "/dev/null") >= riscv::FileDescriptors::FILE_D_BASE);
}

namespace {
struct PollMachine {
	riscv::Machine<RISCV64> machine { std::vector<uint8_t>{}, { .memory_max = MAX_MEMORY } };
	uint64_t g_fds = 0;
	uint64_t g_ts = 0;
	int pipe_fds[2] = { -1, -1 };
	int vfd = -1;

	PollMachine() {
		machine.setup_linux_syscalls(true, false);
		machine.set_max_instructions(~0ULL);
		g_fds = machine.memory.mmap_allocate(4096);
		g_ts  = g_fds + 2048;
		REQUIRE(::pipe(pipe_fds) == 0);
		// The machine owns (and closes) the read end
		vfd = machine.fds().assign_file(pipe_fds[0]);
	}
	~PollMachine() { ::close(pipe_fds[1]); }

	long ppoll(std::vector<struct pollfd> fds, const int64_t* ts) {
		machine.copy_to_guest(g_fds, fds.data(), fds.size() * sizeof(fds[0]));
		if (ts != nullptr)
			machine.copy_to_guest(g_ts, ts, 2 * sizeof(int64_t));
		machine.cpu.reg(riscv::REG_ARG0) = g_fds;
		machine.cpu.reg(riscv::REG_ARG1) = fds.size();
		machine.cpu.reg(riscv::REG_ARG2) = (ts != nullptr) ? g_ts : 0;
		machine.cpu.reg(riscv::REG_ARG3) = 0;
		machine.system_call(73);
		return machine.return_value<long>();
	}
	short revents(unsigned i) {
		struct pollfd pfd;
		machine.copy_from_guest(&pfd, g_fds + i * sizeof(pfd), sizeof(pfd));
		return pfd.revents;
	}
	int64_t remaining_ns() {
		int64_t ts[2];
		machine.copy_from_guest(ts, g_ts, sizeof(ts));
		return ts[0] * 1'000'000'000LL + ts[1];
	}
};
}

TEST_CASE("C14: ppoll with a NULL timeout", "[Security]")
{
	PollMachine m;
	// A ready descriptor and an infinite wait
	REQUIRE(::write(m.pipe_fds[1], "x", 1) == 1);
	REQUIRE(m.ppoll({{ m.vfd, POLLIN, 0 }}, nullptr) == 1);
	REQUIRE((m.revents(0) & POLLIN) != 0);
	char c;
	REQUIRE(::read(m.pipe_fds[0], &c, 1) == 1);

	// Nothing ready and an infinite wait: the host is blocked for a bounded
	// time, which is charged, and the guest sees an interrupted wait.
	const uint64_t counter = m.machine.instruction_counter();
	const auto t0 = std::chrono::steady_clock::now();
	REQUIRE(m.ppoll({{ m.vfd, POLLIN, POLLIN }}, nullptr) == -EINTR);
	const auto elapsed = std::chrono::steady_clock::now() - t0;
	REQUIRE(elapsed < std::chrono::seconds(3));
	REQUIRE(m.machine.instruction_counter() - counter >= 500'000);
	// revents is always written back
	REQUIRE(m.revents(0) == 0);
}

TEST_CASE("C14: ppoll timeouts are validated and bounded", "[Security]")
{
	PollMachine m;
	const int64_t bad_nsec[2] = { 0, 1'000'000'000LL };
	REQUIRE(m.ppoll({{ m.vfd, POLLIN, 0 }}, bad_nsec) == -EINVAL);
	const int64_t negative[2] = { -1, 0 };
	REQUIRE(m.ppoll({{ m.vfd, POLLIN, 0 }}, negative) == -EINVAL);

	// A short timeout that expires is a timeout, with nothing remaining
	const int64_t short_wait[2] = { 0, 10'000'000LL };
	REQUIRE(m.ppoll({{ m.vfd, POLLIN, 0 }}, short_wait) == 0);
	REQUIRE(m.remaining_ns() == 0);

	// A huge timeout does not overflow: it is cut short, and the time that
	// remains is written back
	const int64_t huge[2] = { INT64_MAX, 999'999'999LL };
	const auto t0 = std::chrono::steady_clock::now();
	REQUIRE(m.ppoll({{ m.vfd, POLLIN, 0 }}, huge) == -EINTR);
	REQUIRE(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3));
	REQUIRE(m.remaining_ns() > INT64_MAX / 2);
}

TEST_CASE("C14: ppoll on descriptors the guest does not have", "[Security]")
{
	PollMachine m;
	// Unknown descriptors are invalid (and ready), so an infinite wait
	// returns at once. Negative descriptors are ignored.
	const auto t0 = std::chrono::steady_clock::now();
	REQUIRE(m.ppoll({{ 1234, POLLIN, 0 }, { -1, POLLIN, 0 }, { m.vfd, POLLIN, 0 }}, nullptr) == 1);
	REQUIRE(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500));
	REQUIRE(m.revents(0) == POLLNVAL);
	REQUIRE(m.revents(1) == 0);
	REQUIRE(m.revents(2) == 0);

	// Too many descriptors
	std::vector<struct pollfd> many(129, { -1, 0, 0 });
	REQUIRE(m.ppoll(many, nullptr) == -EINVAL);
}

// =============================================================================
// C15: signal numbers above 64 threw std::out_of_range
//
// Signals::get() used signals.at(sig-1), so a guest sigaction, tkill or
// tgkill with a signal number above 64 escaped as a non-MachineException.
// Every invalid signal number must be an ILLEGAL_OPERATION instead.
// =============================================================================

static void rt_sigaction(riscv::Machine<RISCV64>& machine, int sig, uint64_t act, uint64_t oldact)
{
	machine.cpu.reg(riscv::REG_ARG0) = sig;
	machine.cpu.reg(riscv::REG_ARG1) = act;
	machine.cpu.reg(riscv::REG_ARG2) = oldact;
	machine.system_call(134);
}

static void require_illegal_operation(const std::function<void()>& func)
{
	try {
		func();
		FAIL("Expected a MachineException");
	} catch (const riscv::MachineException& me) {
		REQUIRE(me.type() == riscv::ILLEGAL_OPERATION);
	}
}

TEST_CASE("C15: invalid signal numbers are illegal operations", "[Security]")
{
	const auto binary = load_file(cwd + "/elf/newlib-rv64gb-hello-world");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	machine.setup_linux_syscalls(false, false);
	machine.setup_posix_threads();
	const auto area = machine.memory.mmap_allocate(4096);

	// The valid range is 1 to 64
	rt_sigaction(machine, 1, 0, area);
	REQUIRE(machine.return_value<int>() == 0);
	rt_sigaction(machine, 64, 0, area);
	REQUIRE(machine.return_value<int>() == 0);

	for (const int sig : { 65, 1000, INT32_MAX, -1, INT32_MIN })
	{
		require_illegal_operation([&] { rt_sigaction(machine, sig, 0, area); });
		require_illegal_operation([&] { machine.sigaction(sig); });
		require_illegal_operation([&] { tgkill(machine, riscv::MAIN_THREAD_TID, sig); });
		require_illegal_operation([&] {
			machine.cpu.reg(riscv::REG_ARG0) = riscv::MAIN_THREAD_TID;
			machine.cpu.reg(riscv::REG_ARG1) = sig;
			machine.system_call(130); // tkill
		});
	}
	// Signal 0 remains a no-op for the system calls
	rt_sigaction(machine, 0, 0, area);
	tgkill(machine, riscv::MAIN_THREAD_TID, 0);
	require_illegal_operation([&] { machine.sigaction(0); });
}
