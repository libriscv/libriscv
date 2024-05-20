#include "event.hpp"
#include <chrono>
#include <fmt/printf.h>
using namespace riscv;
using gaddr_t = Script::gaddr_t;
using machine_t = Script::machine_t;
inline Script& script(machine_t& m) {
	return *m.get_userdata<Script>();
}

template <unsigned SAMPLES = 3000>
static void benchmark(std::string_view name, Script& script, std::function<void()> fn)
{
	static unsigned overhead = 0;
	if (overhead == 0) {
		Event<void()> measure_overhead(script, "measure_overhead");
		auto start = std::chrono::high_resolution_clock::now();
		for (unsigned i = 0; i < SAMPLES; i++)
			measure_overhead();
		auto end = std::chrono::high_resolution_clock::now();
		overhead = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / SAMPLES;
		fmt::print("Call overhead: {}ns\n", overhead);
	}

	auto start = std::chrono::high_resolution_clock::now();
	for (unsigned i = 0; i < SAMPLES; i++)
		fn();
	auto end = std::chrono::high_resolution_clock::now();
	auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / SAMPLES;
	fmt::print("Benchmark: {}  Elapsed time: {}ns\n",
		name, elapsed - overhead);
}

int main(int argc, char** argv)
{
	if (argc < 2) {
		fmt::print("Usage: {} [program file] [arguments ...]\n", argv[0]);
		return -1;
	}

	fmt::print("Loading program: {}\n", argv[1]);

	// Create a new script instance, loading and initializing the given program file
	// The programs main() function will be called
	Script script("myscript", argv[1]);

	// Create an event for the 'test1' function with 4 arguments and returns an int
	Event<int(int, int, int, int)> test1(script, "test1");
	if (auto ret = test1(1, 2, 3, 4))
		fmt::print("test1 returned: {}\n", *ret);
	else
		throw std::runtime_error("Failed to call test1!?");

	// Benchmark the test2 function, which allocates and frees 1024 bytes
	Event<void()> test2(script, "test2");
	if (auto ret = test2(); !ret)
		throw std::runtime_error("Failed to call test2!?");

	benchmark("std::make_unique[1024] alloc+free", script, [&] {
		test2();
	});

	// Create an event for the 'test3' function with a single string argument
	// This function will throw an exception, which is immediately caught
	Event<void(std::string)> test3(script, "test3");
	if (auto ret = test3("Oh, no! An exception!"); !ret)
		throw std::runtime_error("Failed to call test3!?");
}
