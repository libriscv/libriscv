#include <catch2/catch_test_macros.hpp>
#include <libriscv/native_heap.hpp>
#include <cstdlib>
#include <vector>
static const uintptr_t BEGIN = 0x1000000;
static const uintptr_t END   = 0x2000000;
#define IS_WITHIN(addr) (addr >= BEGIN && addr < END)

int randInt(int min, int max) {
	return min + (rand() % static_cast<int>(max - min + 1));
}
unsigned randUpto(unsigned max) {
	return rand() % max;
}

struct Allocation {
	uint64_t  addr;
	size_t    size;
};

static Allocation alloc_random(riscv::Arena& arena)
{
	const size_t size = randInt(0, 8000);
	const uintptr_t addr = arena.malloc(size);
	REQUIRE(IS_WITHIN(addr));
	const Allocation a {
		.addr = addr, .size = arena.size(addr)
	};
	REQUIRE(a.size >= size);
	return a;
}

static std::tuple<Allocation, size_t> realloc_random(riscv::Arena& arena, uint64_t addr)
{
	REQUIRE(IS_WITHIN(addr));
	const size_t size = randInt(0, 8000);
	auto [newaddr, len] = arena.realloc(addr, size);
	REQUIRE(IS_WITHIN(newaddr));
	const Allocation a {
		.addr = newaddr, .size = arena.size(newaddr)
	};
	REQUIRE(a.size >= size);
	return {a, size};
}

TEST_CASE("Basic heap usage", "[Heap]")
{
	riscv::Arena arena {BEGIN, END};
	std::vector<Allocation> allocs;

	// General allocation test
	for (int i = 0; i < 100; i++) {
		allocs.push_back(alloc_random(arena));
	}

	for (auto entry : allocs) {
		REQUIRE(arena.size(entry.addr) == entry.size);
	  	REQUIRE(arena.free(entry.addr) == 0);
	}
	REQUIRE(arena.bytes_used() == 0);
	REQUIRE(arena.bytes_free() == END - BEGIN);
	allocs.clear();

	// Randomized allocations
	for (int i = 0; i < 10000; i++)
	{
		const int A = randInt(2, 50);
		for (int a = 0; a < A; a++) {
			allocs.push_back(alloc_random(arena));
			const auto alloc = allocs.back();
			printf("Alloc %lX size: %4zu,  arena size: %4zu\n",
				alloc.addr, alloc.size, arena.size(alloc.addr));
		}
		const int B = std::min(randInt(2, allocs.size()), (int)allocs.size());
		for (int b = 0; b < B; b++) {
			auto& origin = allocs.at(b);
			const auto [alloc, size] = realloc_random(arena, origin.addr);
			printf("Realloc %lX size: %4zu, arena size: %4zu  (origin %lX oldsize %zu)\n",
				alloc.addr, size, alloc.size, origin.addr, origin.size);
			if (alloc.addr == origin.addr) {
				origin.size = alloc.size;
				REQUIRE(arena.size(origin.addr) == origin.size);
			} else {
				// The old allocation has just been freed
				REQUIRE(arena.size(origin.addr) == 0);
				REQUIRE(arena.free(origin.addr) == -1);
				allocs.erase(allocs.begin() + b, allocs.begin() + b + 1);
				// Add the new reallocated address
				allocs.push_back(alloc);
				REQUIRE(arena.size(alloc.addr) == alloc.size);
			}
		}
		const int F = randInt(2, allocs.size());
		for (int f = 0; f < F && !allocs.empty(); f++) {
			const auto idx = randUpto(allocs.size());
			const auto alloc = allocs.at(idx);
			allocs.erase(allocs.begin() + idx, allocs.begin() + idx + 1);
			printf("Free %lX size: %4zu, arena size: %4zu\n",
				alloc.addr, alloc.size, arena.size(alloc.addr));
			REQUIRE(arena.size(alloc.addr) == alloc.size);
	  		REQUIRE(arena.free(alloc.addr) == 0);
		}
	}
	// Verify all allocations still remaining
	for (auto entry : allocs) {
		REQUIRE(arena.size(entry.addr) == entry.size);
	}
	// Verify allocations still remaining, then free them
	for (auto entry : allocs) {
		REQUIRE(arena.size(entry.addr) == entry.size);
		REQUIRE(arena.free(entry.addr) == 0);
		REQUIRE(arena.size(entry.addr) == 0);
	}
	REQUIRE(arena.bytes_used() == 0);
	REQUIRE(arena.bytes_free() == END - BEGIN);
	allocs.clear();
}
