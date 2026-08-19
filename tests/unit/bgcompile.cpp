#include <catch2/catch_test_macros.hpp>

#include <libriscv/machine.hpp>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>
extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2 -static", bool cpp = false);
using namespace riscv;

// Background compilation hands a compilation step to the embedder, which
// typically runs it on a detached thread. The step holds a reference to the
// execute segment, but only a raw pointer to the Machine it was started from,
// which means the Machine must not be allowed to go away first.
static std::atomic<int> g_bg_threads {0};

static void background_callback(std::function<void()>& step)
{
	g_bg_threads++;
	std::thread([step = std::move(step)] {
		// Embedders are free to queue the compilation step and get to it a
		// moment later. Deferring it here makes the interesting case - the
		// machine going away first - happen every time instead of by chance.
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		try {
			step();
		} catch (const std::exception& e) {
			fprintf(stderr, "Background compilation failed: %s\n", e.what());
		}
		g_bg_threads--;
	}).detach();
}

static void wait_for_background_threads()
{
	// Detached threads must not be alive when the process tears down its
	// globals, otherwise the test itself is racy.
	while (g_bg_threads.load() != 0)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

TEST_CASE("Destroy machines while background compiling", "[Bintr]")
{
	if constexpr (!binary_translation_enabled) {
		SUCCEED("Binary translation is not enabled");
		return;
	}

	const auto binary = build_and_load(R"M(
	#include <stdio.h>
	static long fib(long n, long a, long b) {
		if (n == 0) return a;
		return fib(n - 1, b, a + b);
	}
	int main() {
		long sum = 0;
		for (long i = 0; i < 1000; i++)
			sum += fib(25, 0, 1) % (i + 1);
		printf("%ld\n", sum);
		return 0;
	})M");

	// The syscall handler table is global: install it before going wide
	Machine<RISCV64>::setup_minimal_syscalls();

	static constexpr size_t WORKERS = 4;
	static constexpr size_t ITERATIONS = 8;

	auto worker = [&binary] {
		for (size_t i = 0; i < ITERATIONS; i++) {
			MachineOptions<RISCV64> options;
			// Each machine gets its own execute segment and compiles from
			// scratch, so that every iteration really does start a compilation.
			options.use_shared_execute_segments = false;
			options.translation_cache = false;
			options.translate_enabled = true;
			options.translate_invoke_compiler = true;
			options.translate_background_callback = background_callback;

			// Heap-allocated on purpose: a background compilation that outlives
			// the machine then reads freed memory instead of a re-used stack
			// frame, which is what an embedder like a game engine would do.
			auto machine = std::make_unique<Machine<RISCV64>> (binary, options);
			machine->set_printer([] (const Machine<RISCV64>&, const char*, size_t) {});
			try {
				// Stop almost immediately, so that the machine is destroyed
				// while the background compilation is still in flight.
				machine->simulate(1000);
			} catch (const MachineException&) {
				// Running out of instructions here is expected
			}
			machine.reset();
		}
	};

	std::vector<std::thread> threads;
	for (size_t i = 0; i < WORKERS; i++)
		threads.emplace_back(worker);
	for (auto& t : threads)
		t.join();

	wait_for_background_threads();
	REQUIRE(g_bg_threads.load() == 0);
}
