// Regression test: copy_to_guest / Memory::memcpy / Memory::memset must be
// visible through the same path the guest CPU reads (read<T> / memview).
//
// Under the DEFAULT build (RISCV_FLAT_RW_ARENA=ON + RISCV_VIRTUAL_PAGING=ON)
// Memory::read/write and guest loads go through the flat arena directly, while
// Memory::memcpy historically branched on #ifndef RISCV_VIRTUAL_PAGING and
// went through the page table. On a CoW fork every pre-existing page is loaned
// as is_cow+non_owning; the first memcpy triggers Page::make_writable(), which
// allocates a private PageData detached from the arena, so the bytes written
// by memcpy were never visible to the guest (silent data loss).
//
// A guest ELF is required: an empty machine has read_boundary == 0, so both
// paths already agree and the bug cannot be observed.
//
// This test fails before the fix (dual-macro default build only) and passes
// after it, in all three build configurations.
#include <catch2/catch_test_macros.hpp>

#include <libriscv/machine.hpp>

#include <cstring>

extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2 -static", bool cpp = false);

TEST_CASE("Forked machine keeps memcpy writes visible to guest reads", "[fork]")
{
	const auto binary = build_and_load(R"M(
	__attribute__((used, retain))
	unsigned char buffer[16] = "AAAA";
	int main() { return 0; }
	)M");

	riscv::MachineOptions<8> opts;
	opts.memory_max = 32ull << 20;
	riscv::Machine<8> parent(binary, opts);

	const auto buf = parent.address_of("buffer");
	REQUIRE(buf != 0);

	// CoW fork. Note: only makes sense in the default dual-macro build where
	// the child inherits the master's flat arena; this invariant is asserted
	// explicitly below rather than assumed.
	riscv::Machine<8> child(parent, riscv::MachineOptions<8>{});
	REQUIRE(child.memory.uses_flat_memory_arena() == parent.memory.uses_flat_memory_arena());

	char rb[5] = {};

	// Bulk write through the page-table path on the child...
	child.memory.memcpy(buf, "BBBB", 4);
	// ...must be visible through the exact path guest loads use (read<uint8_t>).
	for (size_t i = 0; i < 4; i++)
		rb[i] = (char)child.memory.read<uint8_t>(buf + i);
	CHECK(std::memcmp(rb, "BBBB", 4) == 0);

	child.copy_to_guest(buf, "CCCC", 4);
	for (size_t i = 0; i < 4; i++)
		rb[i] = (char)child.memory.read<uint8_t>(buf + i);
	CHECK(std::memcmp(rb, "CCCC", 4) == 0);

	child.memory.memset(buf, 0x44, 4);
	for (size_t i = 0; i < 4; i++)
		rb[i] = (char)child.memory.read<uint8_t>(buf + i);
	CHECK(std::memcmp(rb, "\x44\x44\x44\x44", 4) == 0);
}
