#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <stdexcept>
#include "type_name.hpp"

inline uint32_t rdcycle()
{
	uint32_t cycle;
	asm volatile ("rdcycle %0" : "=r"(cycle));
	return cycle;
}
inline uint32_t rdtime()
{
	uint32_t t;
	asm volatile ("rdtime %0" : "=r"(t));
	return t;
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

	uint32_t t0 = rdtime();
	for (int i = 0; i < 1; i++)
	{
		uint32_t c0 = rdcycle();
		try {
			throw std::runtime_error("Oh god!");
		}
		catch (std::exception& e) {
			printf("Error: %s\n", e.what());
			write(5, e.what(), strlen(e.what()));
		}
		uint32_t c1 = rdcycle();
		printf("It took %lu cycles to throw, catch and printf exception\n", c1 - c0);
	}
	uint32_t t1 = rdtime();
	uint32_t millis = (t1 - t0) / 1000000ul;
	printf("It took %lu nanos (%lu millis) for the whole thing\n", t1 - t0, millis);

	const char hello_void[] = "Hello Virtual World!\n";
	write(0, hello_void, sizeof(hello_void)-1);




	return 666;
}
