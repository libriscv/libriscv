#include <catch2/catch_test_macros.hpp>
#include <libriscv/machine.hpp>
#include <libriscv/native_heap.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace {
constexpr uint32_t BASE = 0x1000000u;
constexpr uint32_t END = 0x3000000u;

void require_valid(const riscv::Arena& arena)
{
	std::string error;
	const bool valid = arena.validate(&error);
	INFO(error);
	REQUIRE(valid);
}

void require_non_overlapping(const riscv::Arena& arena,
	const std::map<uint32_t, size_t>& live)
{
	uint64_t previous_end = BASE;
	for (const auto& [address, ignored] : live) {
		(void)ignored;
		const size_t actual = arena.size(address);
		REQUIRE(actual >= riscv::Arena::ALIGNMENT);
		REQUIRE(address % riscv::Arena::ALIGNMENT == 0);
		REQUIRE(address >= previous_end);
		REQUIRE(uint64_t(address) + actual <= END);
		previous_end = uint64_t(address) + actual;
	}
}

uint64_t grow_steps(unsigned count)
{
	riscv::Arena arena(BASE, END);
	arena.set_max_chunks(count + 2u);
	arena.reset_search_steps();
	for (unsigned i = 0; i < count; ++i)
		REQUIRE(arena.malloc(16u + (i & 31u) * 16u) != 0);
	return arena.search_steps();
}
}

TEST_CASE("Native heap validated differential fuzz", "[Heap][ArenaInvariant]")
{
	for (uint32_t seed : {1u, 0x12345678u, 0xdeadbeefu}) {
		riscv::Arena arena(BASE, END);
		std::mt19937 random(seed);
		std::map<uint32_t, size_t> live;
		for (unsigned operation = 0; operation < 2500; ++operation) {
			const unsigned choice = random() % 100u;
			if (live.empty() || choice < 48u) {
				const size_t requested = random() % 8192u;
				const uint32_t ptr = (choice & 1u)
					? arena.malloc(requested)
					: arena.seq_alloc_aligned(std::min<size_t>(requested, RISCV_PAGE_SIZE), 16, false);
				if (ptr != 0) live.emplace(ptr, arena.size(ptr));
			} else {
				auto it = live.begin();
				std::advance(it, random() % live.size());
				if (choice < 72u) {
					REQUIRE(arena.free(it->first) == 0);
					live.erase(it);
				} else {
					const uint32_t old = it->first;
					const size_t requested = random() % 16384u;
					const auto [ptr, copied] = arena.realloc(old, requested);
					(void)copied;
					if (ptr != 0) {
						live.erase(it);
						live.emplace(ptr, arena.size(ptr));
					}
				}
			}
			require_valid(arena);
			require_non_overlapping(arena, live);
			REQUIRE(arena.max_probe() < 16u);
		}
		for (const auto& [ptr, size] : live) {
			(void)size;
			REQUIRE(arena.free(ptr) == 0);
		}
		REQUIRE(arena.bytes_free() == END - BASE);
		require_valid(arena);
	}
}

TEST_CASE("Native heap size-class boundaries", "[Heap][ArenaInvariant]")
{
	std::vector<size_t> sizes;
	for (unsigned power = 4; power <= 24; ++power) {
		const size_t boundary = size_t{1} << power;
		for (int delta : {-17, -16, -1, 0, 1, 15, 16, 17})
			if (int64_t(boundary) + delta > 0) sizes.push_back(boundary + delta);
	}
	for (size_t boundary = 256; boundary <= 8192; boundary += 16) {
		for (int delta : {-1, 0, 1}) sizes.push_back(boundary + delta);
	}

	riscv::Arena arena(BASE, END);
	for (const size_t requested : sizes) {
		const auto ptr = arena.malloc(requested);
		REQUIRE(ptr != 0);
		REQUIRE(arena.size(ptr) >= requested);
		require_valid(arena);
		REQUIRE(arena.free(ptr) == 0);
		require_valid(arena);
	}
}

TEST_CASE("Native heap exact fits survive coarse size classes", "[Heap][ArenaInvariant]")
{
	for (const uint32_t size : {528u, 1040u, 1056u}) {
		riscv::Arena arena(BASE, BASE + size);
		REQUIRE(arena.malloc(size) == BASE);
		REQUIRE(arena.bytes_free() == 0);
		require_valid(arena);
	}
}

TEST_CASE("Native heap hostile sizes and pointers fail closed", "[Heap][ArenaHostile]")
{
	riscv::Arena arena(BASE, END);
	const size_t original_free = arena.bytes_free();
	REQUIRE(arena.malloc(std::numeric_limits<size_t>::max()) == 0);
	REQUIRE(arena.malloc(std::numeric_limits<size_t>::max() - 15u) == 0);
	REQUIRE(arena.malloc(size_t(END - BASE) + 1u) == 0);
	REQUIRE(arena.malloc(uint64_t{1} << 32u) == 0);
	const auto ptr = arena.malloc(128);
	REQUIRE(ptr != 0);
	REQUIRE(std::get<0>(arena.realloc(ptr, std::numeric_limits<size_t>::max())) == 0);
	const size_t after_alloc = arena.bytes_free();
	for (uint32_t bad : {BASE - 16u, ptr + 8u, END, 0xdeadbeefu}) {
		REQUIRE(arena.free(bad) == -1);
		REQUIRE(arena.size(bad) == 0);
		REQUIRE(arena.bytes_free() == after_alloc);
		require_valid(arena);
	}
	REQUIRE(arena.free(ptr) == 0);
	REQUIRE(arena.bytes_free() == original_free);
	REQUIRE_THROWS(riscv::Arena(uint64_t{1} << 32u, (uint64_t{1} << 32u) + 4096u, 0));
}

TEST_CASE("Native heap syscall layer rejects hostile guest inputs", "[Heap][ArenaHostile]")
{
	std::vector<uint8_t> empty_elf;
	riscv::Machine<8> machine(empty_elf, {.memory_max = 64u << 20});
	const uint64_t heap = 0x100000u;
	machine.setup_native_heap(500, heap, 8u << 20);
	auto syscall = [&] (size_t number, uint64_t a0, uint64_t a1 = 0) {
		machine.cpu.reg(riscv::REG_ARG0) = a0;
		machine.cpu.reg(riscv::REG_ARG1) = a1;
		machine.system_call(number);
		return machine.return_value<uint64_t>();
	};

	REQUIRE(syscall(500, std::numeric_limits<uint64_t>::max()) == 0);
	REQUIRE(syscall(500, std::numeric_limits<uint64_t>::max() - 15u) == 0);
	REQUIRE(syscall(500, uint64_t{1} << 32u) == 0);
	REQUIRE(syscall(501, uint64_t{1} << 32u, uint64_t{1} << 32u) == 0);
	const uint64_t ptr = syscall(500, 128);
	REQUIRE(ptr != 0);
	REQUIRE(syscall(502, ptr, std::numeric_limits<uint64_t>::max()) == 0);
	const size_t free_before = machine.arena().bytes_free();
	for (uint64_t bad : {uint64_t(heap - 16u), ptr + 8u,
		uint64_t(heap + (8u << 20)), uint64_t(0xdeadbeef)}) {
		REQUIRE_THROWS_AS(syscall(503, bad), riscv::MachineException);
		REQUIRE(machine.arena().bytes_free() == free_before);
		require_valid(machine.arena());
	}
	REQUIRE(syscall(503, ptr) == ptr); // free leaves a0 unspecified, as documented.
	require_valid(machine.arena());
}

TEST_CASE("Native heap cap exception is atomic", "[Heap][ArenaHostile]")
{
	riscv::Arena arena(BASE, END, 32);
	std::vector<uint32_t> live;
	for (unsigned i = 0; i < 31; ++i) live.push_back(arena.malloc(16));
	require_valid(arena);
	REQUIRE_THROWS(arena.malloc(16));
	require_valid(arena);
	REQUIRE(arena.metadata_bytes() < 4096u * 4u);
	for (auto ptr : live) REQUIRE(arena.free(ptr) == 0);
	require_valid(arena);
	REQUIRE(arena.malloc(END - BASE) == BASE);

	riscv::Arena lowered(BASE, END);
	const auto a = lowered.malloc(64);
	const auto b = lowered.malloc(64);
	lowered.set_max_chunks(1);
	REQUIRE_THROWS(lowered.malloc(64));
	REQUIRE(lowered.size(a) == 64);
	REQUIRE(lowered.size(b) == 64);
	require_valid(lowered);
}

TEST_CASE("Native heap permits an exhausted fork tail", "[Heap][ArenaInvariant]")
{
	riscv::Arena parent(BASE, END);
	REQUIRE(parent.malloc(END - BASE) == BASE);
	REQUIRE(parent.high_watermark() == END);

	riscv::Arena fork_tail(parent.high_watermark(), END, 16);
	REQUIRE(fork_tail.bytes_free() == 0);
	REQUIRE(fork_tail.bytes_used() == 0);
	REQUIRE(fork_tail.malloc(16) == 0);
	require_valid(fork_tail);
}

TEST_CASE("Native heap slab growth and transfer remain valid", "[Heap][ArenaInvariant]")
{
	riscv::Arena source(BASE, END);
	source.set_max_chunks(4096);
	std::vector<uint32_t> pointers;
	for (unsigned i = 0; i < 1024; ++i) pointers.push_back(source.malloc(16 + (i % 17) * 16));
	for (unsigned i = 1; i < pointers.size(); i += 3) REQUIRE(source.free(pointers[i]) == 0);
	require_valid(source);

	riscv::Arena copy(source);
	require_valid(copy);
	REQUIRE(copy.bytes_free() == source.bytes_free());
	REQUIRE(copy.high_watermark() == source.high_watermark());
	for (unsigned i = 0; i < pointers.size(); i += 3) {
		if (copy.size(pointers[i])) REQUIRE(copy.free(pointers[i]) == 0);
	}
	require_valid(copy);
	require_valid(source);
}

TEST_CASE("Native heap exact-class LIFO reuse", "[Heap][ArenaInvariant]")
{
	riscv::Arena arena(BASE, END);
	const auto a = arena.malloc(96);
	const auto blocker = arena.malloc(32);
	REQUIRE(arena.free(a) == 0);
	REQUIRE(arena.malloc(96) == a);
	REQUIRE(arena.free(blocker) == 0);
	require_valid(arena);
}

TEST_CASE("Native heap sequential allocation has a bounded fallback", "[Heap][ArenaInvariant]")
{
	riscv::Arena arena(BASE, END);
	std::vector<uint32_t> holes;
	REQUIRE(arena.malloc(3808) != 0); // Place each hole 288 bytes before a page boundary.
	for (unsigned i = 0; i < riscv::Arena::SEQ_PROBE_LIMIT + 1u; ++i) {
		holes.push_back(arena.malloc(512));
		REQUIRE(arena.malloc(3584) != 0); // Restore the same offset on the next page.
	}
	for (auto hole : holes) REQUIRE(arena.free(hole) == 0);
	require_valid(arena);
	const auto before = arena.sequential_fallbacks();
	const auto ptr = arena.seq_alloc_aligned(512, 16, false);
	REQUIRE(ptr != 0);
	REQUIRE((ptr & ~(RISCV_PAGE_SIZE - 1u)) ==
		((ptr + 511u) & ~(RISCV_PAGE_SIZE - 1u)));
	REQUIRE(arena.sequential_fallbacks() == before + 1u);
	require_valid(arena);
}

TEST_CASE("Native heap sequential exact suffix respects the chunk cap", "[Heap][ArenaInvariant]")
{
	constexpr uint32_t BOUNDARY = BASE + RISCV_PAGE_SIZE;
	constexpr uint32_t SEQ_BASE = BOUNDARY - 240u;
	constexpr uint32_t SEQ_END = BOUNDARY + 256u;
	riscv::Arena arena(SEQ_BASE, SEQ_END, 2);

	REQUIRE(arena.seq_alloc_aligned(256, 16, false) == BOUNDARY);
	require_valid(arena);
}

TEST_CASE("Native heap search work is independent of live chunk count", "[Heap][ArenaPerformance]")
{
	const uint64_t small = grow_steps(1'000);
	const uint64_t large = grow_steps(8'000);
	INFO(small);
	INFO(large);
	REQUIRE(small <= 3u * 1'000u);
	REQUIRE(large <= 3u * 8'000u);
	REQUIRE(double(large) / 8'000.0 <= double(small) / 1'000.0 + 0.25);
}

TEST_CASE("Native heap accounting queries are constant work", "[Heap][ArenaPerformance]")
{
	riscv::Arena arena(BASE, END);
	arena.set_max_chunks(65'536);
	for (unsigned i = 0; i < 32'000; ++i) REQUIRE(arena.malloc(16) != 0);
	arena.reset_search_steps();
	const volatile size_t free_bytes = arena.bytes_free();
	const volatile size_t used_bytes = arena.bytes_used();
	const volatile uint32_t watermark = arena.high_watermark();
	(void)free_bytes;
	(void)used_bytes;
	(void)watermark;
	REQUIRE(arena.search_steps() == 0);
}

TEST_CASE("Native heap exhaustion fully recovers", "[Heap][ArenaInvariant]")
{
	constexpr uint32_t SMALL_END = BASE + 64u * 1024u;
	riscv::Arena arena(BASE, SMALL_END);
	std::vector<uint32_t> pointers;
	while (const auto ptr = arena.malloc(48)) pointers.push_back(ptr);
	for (const auto ptr : pointers) REQUIRE(arena.free(ptr) == 0);
	require_valid(arena);
	REQUIRE(arena.bytes_free() == SMALL_END - BASE);
	REQUIRE(arena.malloc(SMALL_END - BASE) == BASE);
}
