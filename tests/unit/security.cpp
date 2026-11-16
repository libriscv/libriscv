#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libriscv/machine.hpp>
#include <libriscv/threads.hpp>
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
