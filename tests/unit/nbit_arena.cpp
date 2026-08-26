#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libriscv/machine.hpp>
extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2 -static", bool cpp = false);
static const uint64_t MAX_MEMORY = 32ul << 20;
static const uint64_t MAX_INSTRUCTIONS = 10'000'000ul;
using namespace riscv;

static const std::string writer = R"M(
#include <stdint.h>
#include <stdio.h>
const int32_t rodata_value = 42;
static int32_t sink;

__attribute__((noinline)) void poke(int32_t *dst, int32_t value) { *dst = value; }
__attribute__((noinline)) int32_t peek(const int32_t *src) { return *src; }
__attribute__((noinline)) const int32_t *rodata_addr(void) { return &rodata_value; }

int main(void) {
	poke(&sink, 7);
	printf("%d %d\n", peek(&sink), *rodata_addr());
	return 0;
}
)M";

TEST_CASE("An automatic N-bit address space masks arena accesses", "[Nbit]")
{
	const auto binary = build_and_load(writer);

	const auto run = [&] (bool nbit) {
		Machine<RISCV64> machine { binary, {
			.memory_max = MAX_MEMORY,
			.translate_automatic_nbit_address_space = nbit,
		} };
		machine.setup_linux({"nbit_arena"}, {"LC_ALL=C"});
		machine.setup_linux_syscalls();
		machine.simulate(MAX_INSTRUCTIONS);
		REQUIRE(machine.memory.uses_flat_memory_arena());
		return machine;
	};

	SECTION("A bounds-checked build faults on a read-only store")
	{
		auto machine = run(false);
		const auto addr = machine.vmcall("rodata_addr");
		REQUIRE(int32_t(machine.vmcall("peek", addr)) == 42);

		REQUIRE_THROWS_WITH([&] {
			machine.vmcall("poke", addr, 43);
		}(), Catch::Matchers::ContainsSubstring("Protection fault"));
	}

	SECTION("A masked build performs it, and stays correct elsewhere")
	{
		auto machine = run(true);
		const auto addr = machine.vmcall("rodata_addr");
		REQUIRE(int32_t(machine.vmcall("peek", addr)) == 42);

		machine.vmcall("poke", addr, 43);
		REQUIRE(int32_t(machine.vmcall("peek", addr)) == 43);
	}
}
