#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libriscv/machine.hpp>
#include <cerrno>
extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2 -static", bool cpp = false);
using namespace riscv;
static const std::vector<uint8_t> empty;
static constexpr uint32_t V = 0x1000;
static constexpr uint32_t VLEN = 16*Page::size();

TEST_CASE("Basic page protections", "[Memory]")
{
	Machine<RISCV32> machine { empty, {.use_memory_arena = false} };

	machine.memory.set_page_attr(V, VLEN, {.read = false, .write = true, .exec = false});
	machine.memory.memset(V, 0, VLEN);
	machine.memory.set_page_attr(V, VLEN, {.read = false, .write = false, .exec = true});

	machine.cpu.jump(V);
	REQUIRE(machine.cpu.pc() == V);
	// The data at V is all zeroes, which forms an
	// illegal instruction in RISC-V.
	REQUIRE_THROWS_WITH([&] {
		machine.simulate(1);
	}(), Catch::Matchers::ContainsSubstring("Illegal opcode executed"));

	// V is not readable anymore
	REQUIRE_THROWS_WITH([&] {
		machine.memory.membuffer(V, VLEN);
	}(), Catch::Matchers::ContainsSubstring("Protection fault"));

	REQUIRE_THROWS_WITH([&] {
		machine.memory.memview(V, VLEN);
	}(), Catch::Matchers::ContainsSubstring("Protection fault"));

	REQUIRE_THROWS_WITH([&] {
		machine.memory.memstring(V);
	}(), Catch::Matchers::ContainsSubstring("Protection fault"));

	// V is not writable anymore
	REQUIRE_THROWS_WITH([&] {
		machine.memory.memset(V, 0, VLEN);
	}(), Catch::Matchers::ContainsSubstring("Protection fault"));

	REQUIRE_THROWS_WITH([&] {
		machine.memory.memcpy(V, "1234", 4);
	}(), Catch::Matchers::ContainsSubstring("Protection fault"));
}

TEST_CASE("Trigger guard pages", "[Memory]")
{
	Machine<RISCV32> machine { empty };

	machine.memory.install_shared_page(0, riscv::Page::guard_page());
	machine.memory.install_shared_page(17, riscv::Page::guard_page());
	machine.memory.memset(V, 0, VLEN);

	// V is not executable
	REQUIRE_THROWS_WITH([&] {
		machine.cpu.jump(V);
		machine.simulate(1);
	}(), Catch::Matchers::ContainsSubstring("Execution space protection fault"));

	// Guard pages are not writable
	REQUIRE_THROWS_WITH([&] {
		machine.memory.memset(V-4, 0, 4);
	}(), Catch::Matchers::ContainsSubstring("Protection fault"));
	REQUIRE_THROWS_WITH([&] {
		machine.memory.memset(V+16*Page::size(), 0, 4);
	}(), Catch::Matchers::ContainsSubstring("Protection fault"));
}

TEST_CASE("Test misaligned page attributes", "[Memory]")
{
	Machine<RISCV32> machine { empty };

	machine.memory.memset(V, 0, VLEN);
	machine.memory.set_page_attr(V + 4095, 2,
		{.read = false, .write = false, .exec = false});
	REQUIRE(machine.memory.get_page(V + 0).attr.read == false);
	REQUIRE(machine.memory.get_page(V + 4095).attr.read == false);
	REQUIRE(machine.memory.get_page(V + 4096).attr.read == false);
	REQUIRE(machine.memory.get_page(V + 8191).attr.read == false);
	REQUIRE(machine.memory.get_page(V + 8192).attr.read == true);
	machine.memory.set_page_attr(V + 4095, 1,
		{.read = true, .write = true, .exec = true});
	REQUIRE(machine.memory.get_page(V + 0).attr.read == true);
	REQUIRE(machine.memory.get_page(V + 4095).attr.read == true);
	REQUIRE(machine.memory.get_page(V + 4096).attr.read == false);
	REQUIRE(machine.memory.get_page(V + 8191).attr.read == false);
	REQUIRE(machine.memory.get_page(V + 8192).attr.read == true);
}

TEST_CASE("Caches must be invalidated", "[Memory]")
{
	// Test not supported on flat read-write arena
	if constexpr (riscv::flat_readwrite_arena)
		return;

	Machine<RISCV32> machine { empty };

	// Force creation of writable pages
	machine.memory.memset(V, 0, VLEN);
	// Read data from page, causing cached read
	REQUIRE(machine.memory.read<uint32_t> (V) == 0x0);
	// Make page completely unpresented
	machine.memory.set_page_attr(V, Page::size(), {.read = false, .write = false, .exec = false});
	// We can still read from the page, because
	// it is in the read cache.
	REQUIRE(machine.memory.read<uint32_t> (V) == 0x0);

	// Invalidate the caches
	machine.memory.invalidate_reset_cache();

	// We can no longer read from the page
	REQUIRE_THROWS_WITH([&] {
		machine.memory.read<uint32_t> (V);
	}(), Catch::Matchers::ContainsSubstring("Protection fault"));
}


TEST_CASE("Writes to read-only segment", "[Memory]")
{
	static const uint64_t MAX_MEMORY = 8ul << 20; /* 8MB */
	static const uint64_t MAX_INSTRUCTIONS = 10'000'000ul;

	const auto binary = build_and_load(R"M(
	static const int array[4] = {1, 2, 3, 4};
	__attribute__((optimize("-O0")))
	int main() {
		*(volatile int *)array = 1234;

		if (array[0] != 1234)
			return -1;
		return 666;
	}
	__attribute__((used, retain))
	void write_to(char* dst) {
		*dst = 1;
	}
	__attribute__((used, retain))
	int read_from(char* dst) {
		return *dst;
	})M");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	// We need to install Linux system calls for maximum gucciness
	machine.setup_linux_syscalls();
	// We need to create a Linux environment for runtimes to work well
	machine.setup_linux(
		{"rodata"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	REQUIRE_THROWS_WITH([&] {
		machine.memory.write<uint8_t>(machine.cpu.pc(), 0);
	}(), Catch::Matchers::ContainsSubstring("Protection fault"));

	// Guard pages are not writable
	REQUIRE_THROWS_WITH([&] {
		machine.simulate(MAX_INSTRUCTIONS);
	}(), Catch::Matchers::ContainsSubstring("Protection fault"));

	REQUIRE(machine.return_value<int>() != 666);

	const auto write_addr = machine.address_of("write_to");
	REQUIRE(write_addr != 0x0);
	const auto read_addr = machine.address_of("read_from");
	REQUIRE(read_addr != 0x0);

	// Reads amd writes to invalid locations
	REQUIRE_THROWS_WITH([&] {
		machine.vmcall<MAX_INSTRUCTIONS>(read_addr, 0);
	}(), Catch::Matchers::ContainsSubstring("Protection fault"));

	machine.vmcall<MAX_INSTRUCTIONS>(read_addr, 0x1000);

	for (uint64_t addr = machine.memory.start_address(); addr < machine.memory.initial_rodata_end(); addr += 0x1000) {
		REQUIRE_THROWS_WITH([&] {
			machine.vmcall<MAX_INSTRUCTIONS>(write_addr, addr);
		}(), Catch::Matchers::ContainsSubstring("Protection fault"));
	}
}

TEST_CASE("Page attributes are bounded by memory_max", "[Memory]")
{
	static constexpr uint64_t MEMORY_MAX = 8ul << 20; /* 8MB */
	static constexpr size_t PAGES_MAX =
		(MEMORY_MAX / Page::size()) * Memory<RISCV64>::PAGE_TABLE_OVERCOMMIT;

	Machine<RISCV64> machine { empty, {
		.memory_max = MEMORY_MAX, .use_memory_arena = false
	} };
	REQUIRE(machine.memory.pages_max() == PAGES_MAX);

	// Setting attributes on an enormous range must not be able to
	// exhaust host memory by creating one page per page in the range.
	REQUIRE_THROWS_WITH([&] {
		machine.memory.set_page_attr(0x100000, 1ull << 40, {.read = true, .write = false});
	}(), Catch::Matchers::ContainsSubstring("Out of memory"));

	REQUIRE(machine.memory.pages_active() <= PAGES_MAX);
}

TEST_CASE("mprotect cannot exhaust host memory", "[Memory]")
{
	static constexpr uint64_t MEMORY_MAX = 8ul << 20; /* 8MB */
	static constexpr size_t PAGES_MAX =
		(MEMORY_MAX / Page::size()) * Memory<RISCV64>::PAGE_TABLE_OVERCOMMIT;
	static constexpr unsigned SYSCALL_MPROTECT = 226;

	Machine<RISCV64> machine { empty, {
		.memory_max = MEMORY_MAX, .use_memory_arena = false
	} };
	machine.setup_linux_syscalls(false, false);
	auto& cpu = machine.cpu;

	// Unaligned address is rejected outright
	cpu.reg(riscv::REG_ARG0) = 0x100001;
	cpu.reg(riscv::REG_ARG1) = Page::size();
	cpu.reg(riscv::REG_ARG2) = 0x3; // PROT_READ | PROT_WRITE
	machine.system_call(SYSCALL_MPROTECT);
	REQUIRE(machine.return_value<int>() == -EINVAL);

	// A length that overflows the address space is rejected
	cpu.reg(riscv::REG_ARG0) = 0x100000;
	cpu.reg(riscv::REG_ARG1) = ~uint64_t(0) - 0x1000;
	cpu.reg(riscv::REG_ARG2) = 0x3;
	machine.system_call(SYSCALL_MPROTECT);
	REQUIRE(machine.return_value<int>() == -ENOMEM);

	// A huge, but non-overflowing length runs out of memory instead of
	// allocating a page-table entry for every page in the range.
	cpu.reg(riscv::REG_ARG0) = 0x100000;
	cpu.reg(riscv::REG_ARG1) = 1ull << 40;
	cpu.reg(riscv::REG_ARG2) = 0x1; // PROT_READ (non-default, creates pages)
	REQUIRE_THROWS_WITH([&] {
		machine.system_call(SYSCALL_MPROTECT);
	}(), Catch::Matchers::ContainsSubstring("Out of memory"));

	REQUIRE(machine.memory.pages_active() <= PAGES_MAX);
}

TEST_CASE("Freeing an enormous range only frees the range", "[Memory]")
{
	static constexpr uint64_t MEMORY_MAX = 8ul << 20; /* 8MB */
	static constexpr uint64_t A = 0x100000;
	static constexpr uint64_t B = 1ull << 42;

	Machine<RISCV64> machine { empty, {
		.memory_max = MEMORY_MAX, .use_memory_arena = false
	} };
	const size_t before = machine.memory.pages_active();

	machine.memory.set_page_attr(A, 4 * Page::size(), {.read = true, .write = false});
	machine.memory.set_page_attr(B, 2 * Page::size(), {.read = true, .write = false});
	REQUIRE(machine.memory.pages_active() == before + 6);

	// Freeing a range much larger than the page table must not visit every
	// page in the range, and must leave pages outside the range alone.
	machine.memory.free_pages(A, 1ull << 40);
	REQUIRE(machine.memory.pages_active() == before + 2);
	REQUIRE(machine.memory.get_page(B).attr.read == true);

	machine.memory.free_pages(B, 2 * Page::size());
	REQUIRE(machine.memory.pages_active() == before);
}
