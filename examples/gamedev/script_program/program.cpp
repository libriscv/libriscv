#include "api.hpp"
#include <cstdio>
#include <memory>
#define PUBLIC(x) extern "C" __attribute__((used, retain)) x

/* Every instantiated program runs through main() */
int main(int argc, char** argv)
{
	std::set_terminate(
	[] {
		try {
			std::rethrow_exception(std::current_exception());
		}
		catch (const std::exception& e) {
			printf("Uncaught exception: %s\n", e.what());
		}
	});

	// Printf uses an internal buffer, so we need to flush it
	printf("Hello, World from a RISC-V virtual machine!\n");
	fflush(stdout);
	// Let's avoid calling global destructors
	_exit(0);
}

PUBLIC(int test1(int a, int b, int c, int d))
{
	printf("test1(%d, %d, %d, %d)\n", a, b, c, d);
	return a + b + c + d;
}

PUBLIC(void test2())
{
	auto x = std::make_unique_for_overwrite<char[]>(1024);
	__asm__("" :: "m"(x[0]) : "memory");
}

PUBLIC(void test3(const char* msg))
{
	try {
		throw std::runtime_error(msg);
	} catch (const std::exception& e) {
		printf("Caught exception: %s\n", e.what());
		fflush(stdout);
	}
}
