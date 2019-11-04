#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <stdexcept>
#include "type_name.hpp"

inline uint32_t rdcycle()
{
	union {
		uint64_t whole;
		uint32_t word[2];
	};
	asm ("rdcycleh %0\n rdcycle %1\n" : "=r"(word[1]), "=r"(word[0]));
	return whole;
}
inline uint64_t rdtime()
{
	union {
		uint64_t whole;
		uint32_t word[2];
	};
	asm ("rdtimeh %0\n rdtime %1\n" : "=r"(word[1]), "=r"(word[0]));
	return whole;
}

int main (int argc, char *argv[])
{
	for (int i = 0; i < argc; i++) {
		printf("arg%d: %s\n", i, argv[i]);
	}

	auto* ptr = new char[1024*1024];
	printf("type of ptr: %s\n", TYPE_NAME(ptr).to_string().c_str());
	// 7-10ms to clear 1mb
	std::memset(ptr, 0, 1024*1024);

	uint64_t t0 = rdtime();
	uint64_t c0 = rdcycle();
	for (int i = 0; i < 2; i++)
	{
		try {
			throw std::runtime_error("Oh god!");
		}
		catch (std::exception& e) {
			//printf("Error: %s\n", e.what());
			//write(5, e.what(), strlen(e.what()));
		}
	}
	uint64_t c1 = rdcycle();
	uint64_t t1 = rdtime();
	printf("It took %llu cycles to throw, catch and printf exception\n", c1 - c0);
	uint32_t millis = (t1 - t0) / 1000000ull;
	printf("It took %lu millis for the whole thing\n", millis);

	const char hello_void[] = "Hello Virtual World!\n";
	write(0, hello_void, sizeof(hello_void)-1);
	return 666;
}
