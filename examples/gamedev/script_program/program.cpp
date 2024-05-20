#include "api.hpp"
#include "dyncall.hpp"
#include <cstdio>
#include <memory>

// A dynamic call for testing integer arguments and return values
DEFINE_DYNCALL(1, dyncall1, int(int));
// A dynamic call for testing string arguments
DEFINE_DYNCALL(2, dyncall2, void(const char*, size_t, const char*));
// A dynamic call for benchmarking the overhead of dynamic calls
DEFINE_DYNCALL(3, dyncall_empty, void());

// Every instantiated program runs through main()
int main(int argc, char** argv)
{
	printf("Hello, World from a RISC-V virtual machine!\n");

	// Call a function that was registered as a dynamic call
	auto result = dyncall1(0x12345678);
	printf("dyncall1(1) = %d\n", result);

	// Call a function that passes a string (with length)
	dyncall2("Hello, Vieworld!", 16, "A zero-terminated string!");

	// Printf uses an internal buffer, so we need to flush it
	fflush(stdout);

	// Let's avoid calling global destructors, as they have a tendency
	// to make global variables unusable before we're done with them.
	// Remember, we're going to be making function calls after this.
	_exit(0);
}

// A PUBLIC() function can be called from the host using script.call("test1"), or an event.
PUBLIC(int test1(int a, int b, int c, int d))
{
	printf("test1(%d, %d, %d, %d)\n", a, b, c, d);
	return a + b + c + d;
}

// This function tests that heap operations are optimized.
PUBLIC(void test2())
{
	auto x = std::make_unique_for_overwrite<char[]>(1024);
	__asm__("" :: "m"(x[0]) : "memory");
}

// This shows that we can catch exceptions. We can't handle unhandled exceptions, outside of main().
PUBLIC(void test3(const char* msg))
{
	try {
		throw std::runtime_error(msg);
	} catch (const std::exception& e) {
		printf("Caught exception: %s\n", e.what());
		fflush(stdout);
	}
}

struct Data {
	int a, b, c, d;
	float e, f, g, h;
	double i, j, k, l;
	char buffer[32];
};
PUBLIC(void test4(const Data& data))
{
	printf("Data: %d %d %d %d %f %f %f %f %f %f %f %f %s\n",
		data.a, data.b, data.c, data.d,
		data.e, data.f, data.g, data.h,
		data.i, data.j, data.k, data.l,
		data.buffer);
	fflush(stdout);
}

// This function is used to benchmark the overhead of dynamic calls.
PUBLIC(void bench_dyncall_overhead())
{
	dyncall_empty();
}
