#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libriscv/machine.hpp>
using namespace riscv;
static const std::vector<uint8_t> empty;
static constexpr uint32_t V = 0x1000;
static constexpr uint32_t VLEN = 16*Page::size();

TEST_CASE("Basic page protections", "[Memory]")
{
	Machine<RISCV32> machine { empty };

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
		machine.memory.memview(V, VLEN,
			[] (const auto&, const uint8_t*, size_t) {
			});
	}(), Catch::Matchers::ContainsSubstring("Protection fault"));

	// V is not writable anymore
	REQUIRE_THROWS_WITH([&] {
		machine.memory.memset(V, 0, VLEN);
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
