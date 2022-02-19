#include <libriscv/native_heap.hpp>
#include <cassert>
#include <cstdlib>
#include <vector>
static const uintptr_t BEGIN = 0x1000000;
static const uintptr_t END   = 0x2000000;

inline bool is_within(uintptr_t addr) {
	return addr >= BEGIN && addr < END;
}

int randInt(int min, int max) {
	return min + (rand() % static_cast<int>(max - min + 1));
}

struct Allocation {
	uintptr_t addr;
	size_t    size;
};

static Allocation alloc_random(riscv::Arena& arena)
{
	const size_t size = randInt(0, 8000);
	const uintptr_t addr = arena.malloc(size);
	assert(is_within(addr));
	const Allocation a {
		.addr = addr, .size = arena.size(addr)
	};
	assert(a.size >= size);
	return a;
}

int main()
{
	riscv::Arena arena {BEGIN, END};
	std::vector<Allocation> allocs;

	// General allocation test
	for (int i = 0; i < 100; i++) {
		allocs.push_back(alloc_random(arena));
	}

	for (auto entry : allocs) {
		assert(arena.size(entry.addr) == entry.size);
	  	assert(arena.free(entry.addr) == 0);
	}
	assert(arena.bytes_used() == 0);
	assert(arena.bytes_free() == END - BEGIN);
	allocs.clear();

	// Randomized allocations
	for (int i = 0; i < 10000; i++)  {
		const int A = randInt(2, 50);
		for (int a = 0; a < A; a++) {
			allocs.push_back(alloc_random(arena));
			const auto alloc = allocs.back();
			printf("Alloc %#lX size: %zu,  arena size: %zu\n",
				alloc.addr, alloc.size, arena.size(alloc.addr));
		}
		const int F = randInt(2, 50);
		for (int f = 0; f < F; f++) {
			if (!allocs.empty()) {
				const auto alloc = allocs.back();
				allocs.pop_back();
				printf("Free %#lX size: %zu,  arena size: %zu\n",
					alloc.addr, alloc.size, arena.size(alloc.addr));
				assert(arena.size(alloc.addr) == alloc.size);
		  		assert(arena.free(alloc.addr) == 0);
			}
		}
	}

	for (auto entry : allocs) {
		assert(arena.size(entry.addr) == entry.size);
	  	assert(arena.free(entry.addr) == 0);
	}
	assert(arena.bytes_used() == 0);
	assert(arena.bytes_free() == END - BEGIN);
	allocs.clear();

	printf("OK\n");
}

void* operator new[](size_t size, const char*, int, unsigned, const char*, int) {
	return ::operator new[] (size);
}
void* operator new[](size_t size, size_t, size_t, const char*, int, unsigned, const char*, int) {
	return ::operator new[] (size);
}
