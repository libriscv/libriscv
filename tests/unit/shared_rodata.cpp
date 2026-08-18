#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libriscv/machine.hpp>
#include <cstdio>
#include <fstream>
extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2 -static", bool cpp = false);
static const uint64_t MAX_MEMORY = 64ul << 20; /* 64MB, room for brk and stack */
static const uint64_t MAX_INSTRUCTIONS = 10'000'000ul;
using namespace riscv;

// An encompassing arena stores without consulting the write boundary, so a
// read-only mapping turns a guest store into a host crash: sharing is off there.
static constexpr bool sharing_enabled = riscv::encompassing_Nbit_arena == 0;

static std::vector<uint8_t> rodata_program()
{
	return build_and_load(R"M(
	#include <stdio.h>
	static const char message[] =
		"The quick brown fox jumps over the lazy dog, repeatedly and at length.";
	__attribute__((used, retain))
	const char* get_message() {
		return message;
	}
	__attribute__((used, retain))
	void write_to(char* dst) {
		*dst = 1;
	}
	int main() {
		printf("%s\n", message);
		return 666;
	})M");
}

static void setup(Machine<RISCV64>& machine)
{
	machine.setup_linux_syscalls();
	machine.setup_linux({"rodata"}, {"LC_TYPE=C", "LC_ALL=C", "USER=root"});
}

// The line in /proc/self/maps that covers addr, or an empty string
static std::string maps_line_for(const void* addr)
{
	std::ifstream file("/proc/self/maps");
	std::string line;
	while (std::getline(file, line)) {
		unsigned long lo = 0, hi = 0;
		if (sscanf(line.c_str(), "%lx-%lx", &lo, &hi) == 2)
			if ((unsigned long)addr >= lo && (unsigned long)addr < hi)
				return line;
	}
	return {};
}

TEST_CASE("Read-only memory is shared between machines", "[Memory]")
{
	if constexpr (!sharing_enabled)
		return;

	const auto binary = rodata_program();

	Machine<RISCV64> first { binary, { .memory_max = MAX_MEMORY } };
	setup(first);

	// The first machine creates the image, and the ones after it map that same
	// image, without ever loading the segments themselves
	const auto shared_end = first.memory.shared_rodata_end();
	REQUIRE(shared_end != 0x0);
	REQUIRE(shared_end % riscv::Page::size() == 0);
	REQUIRE(shared_end <= first.memory.initial_rodata_end());
	// The page holding the end of the read-only data is writable above
	// initial_rodata_end, so it can never be part of the image
	REQUIRE(shared_end <= (first.memory.initial_rodata_end() & ~uint64_t(riscv::Page::size()-1)));

	Machine<RISCV64> second { binary, { .memory_max = MAX_MEMORY } };
	setup(second);

	REQUIRE(second.memory.shared_rodata_end() == shared_end);
	REQUIRE(second.memory.memory_arena_ptr() != first.memory.memory_arena_ptr());
	// Both machines see the same read-only data
	REQUIRE(0 == std::memcmp(first.memory.memory_arena_ptr(),
		second.memory.memory_arena_ptr(), shared_end));

	// And it really is one mapping: same inode, mapped shared and read-only
	const auto line1 = maps_line_for(first.memory.memory_arena_ptr());
	const auto line2 = maps_line_for(second.memory.memory_arena_ptr());
	REQUIRE_FALSE(line1.empty());
	REQUIRE_FALSE(line2.empty());
	REQUIRE_THAT(line1, Catch::Matchers::ContainsSubstring("r--s"));
	REQUIRE_THAT(line2, Catch::Matchers::ContainsSubstring("r--s"));
	REQUIRE_THAT(line1, Catch::Matchers::ContainsSubstring("libriscv-rodata"));
	REQUIRE_THAT(line2, Catch::Matchers::ContainsSubstring("libriscv-rodata"));

	// A machine that skipped loading its read-only segments still runs
	second.simulate(MAX_INSTRUCTIONS);
	REQUIRE(second.return_value<int>() == 666);

	first.simulate(MAX_INSTRUCTIONS);
	REQUIRE(first.return_value<int>() == 666);
}

TEST_CASE("Shared read-only memory can be disabled", "[Memory]")
{
	const auto binary = rodata_program();

	Machine<RISCV64> machine { binary,
		{ .memory_max = MAX_MEMORY, .use_shared_rodata = false } };
	setup(machine);

	REQUIRE(machine.memory.shared_rodata_end() == 0x0);
	REQUIRE(machine.memory.initial_rodata_end() != 0x0);

	machine.simulate(MAX_INSTRUCTIONS);
	REQUIRE(machine.return_value<int>() == 666);
}

TEST_CASE("Shared read-only memory is still write protected", "[Memory]")
{
	if constexpr (!sharing_enabled)
		return;

	const auto binary = rodata_program();

	Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	setup(machine);
	REQUIRE(machine.memory.shared_rodata_end() != 0x0);

	const auto write_addr = machine.address_of("write_to");
	REQUIRE(write_addr != 0x0);

	// The write boundary refuses these before they reach the read-only mapping
	for (uint64_t addr = machine.memory.start_address();
		addr < machine.memory.shared_rodata_end(); addr += 0x1000)
	{
		REQUIRE_THROWS_WITH([&] {
			machine.vmcall<MAX_INSTRUCTIONS>(write_addr, addr);
		}(), Catch::Matchers::ContainsSubstring("Protection fault"));
	}

	// The host is refused as well
	REQUIRE_THROWS_WITH([&] {
		machine.copy_to_guest(machine.memory.start_address(), "x", 1);
	}(), Catch::Matchers::ContainsSubstring("Protection fault"));
}

TEST_CASE("Discarding memory never touches read-only data", "[Memory]")
{
	const auto binary = rodata_program();

	Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	setup(machine);

	const auto rodata_end = machine.memory.initial_rodata_end();
	REQUIRE(rodata_end != 0x0);

	std::vector<uint8_t> before(rodata_end);
	std::memcpy(before.data(), machine.memory.memory_arena_ptr(), rodata_end);

	// A guest can ask for any range to be discarded, including its own rodata
	machine.memory.memdiscard(0x1000, rodata_end - 0x1000, true);

	REQUIRE(0 == std::memcmp(before.data(), machine.memory.memory_arena_ptr(), rodata_end));
}
