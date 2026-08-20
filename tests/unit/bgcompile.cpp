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

#ifdef RISCV_BINARY_TRANSLATION
TEST_CASE("Destroy machines while background compiling", "[Bintr]")
{

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

TEST_CASE("Live-patch a running machine with background binary translation", "[Bintr]")
{
	// Long enough that the machine is still running when the background
	// translation completes, which is the case live-patching exists for.
	const auto binary = build_and_load(R"M(
	#include <stdio.h>
	static long fib(long n, long a, long b) {
		if (n == 0) return a;
		return fib(n - 1, b, a + b);
	}
	int main() {
		long sum = 0;
		for (long i = 0; i < 20000; i++)
			sum += fib(25, 0, 1) % (i + 1);
		printf("%ld\n", sum);
		return 0;
	})M");

	Machine<RISCV64>::setup_minimal_syscalls();

	// The printer is a plain function pointer, so the output has to live outside.
	static std::string g_bt_output;
	auto run = [&binary] (bool background) -> std::string {
		MachineOptions<RISCV64> options;
		options.use_shared_execute_segments = false;
		options.translate_enabled = true;
		options.translate_invoke_compiler = true;
		options.translation_cache = false;
	#ifdef RISCV_ASMJIT
		options.asmjit_enabled = false;
	#endif
		if (background)
			options.translate_background_callback = background_callback;

		g_bt_output.clear();
		Machine<RISCV64> machine { binary, options };
		// A static binary runs its own libc startup, so it needs a real Linux
		// environment and syscall layer to reach main() at all.
		machine.setup_linux({"bgcompile"}, {"LC_ALL=C"});
		machine.setup_linux_syscalls();
		machine.set_printer([] (const Machine<RISCV64>&, const char* p, size_t len) {
			g_bt_output.append(p, len);
		});
		machine.simulate(20'000'000'000ULL);
		REQUIRE(machine.return_value<int>() == 0);
		return g_bt_output;
	};

	const std::string interpreted = run(false);
	REQUIRE(!interpreted.empty());
	// The same program, translated on another thread and live-patched into the
	// running execution, must produce exactly the same output.
	REQUIRE(run(true) == interpreted);

	wait_for_background_threads();
	REQUIRE(g_bg_threads.load() == 0);
}
#endif // RISCV_BINARY_TRANSLATION

#ifdef RISCV_ASMJIT
TEST_CASE("Destroy machines while background translating with asmjit", "[Asmjit]")
{

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

	Machine<RISCV64>::setup_minimal_syscalls();

	static constexpr size_t WORKERS = 4;
	static constexpr size_t ITERATIONS = 4;

	auto worker = [&binary] {
		for (size_t i = 0; i < ITERATIONS; i++) {
			MachineOptions<RISCV64> options;
			// Each machine gets its own execute segment and translates from
			// scratch, so that every iteration really does start a translation.
			options.use_shared_execute_segments = false;
		#ifdef RISCV_BINARY_TRANSLATION
			options.translate_enabled = false;
		#endif
			options.asmjit_enabled = true;
			// What is under test here is the lifetime, not the coverage, and
			// emitting a whole static binary 16 times over is needlessly slow.
			options.asmjit_instr_max = 20000;
			options.asmjit_background_callback = background_callback;

			// Heap-allocated on purpose: a background translation that outlives
			// the machine then reads freed memory instead of a re-used stack
			// frame, which is what an embedder like a game engine would do.
			auto machine = std::make_unique<Machine<RISCV64>> (binary, options);
			machine->set_printer([] (const Machine<RISCV64>&, const char*, size_t) {});
			try {
				// Stop almost immediately, so that the machine is destroyed
				// while the background translation is still in flight.
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

TEST_CASE("Live-patch a running machine with background asmjit", "[Asmjit]")
{
	// Long enough that the machine is still running when the background
	// translation completes, which is the case live-patching exists for.
	const auto binary = build_and_load(R"M(
	#include <stdio.h>
	static long fib(long n, long a, long b) {
		if (n == 0) return a;
		return fib(n - 1, b, a + b);
	}
	int main() {
		long sum = 0;
		for (long i = 0; i < 20000; i++)
			sum += fib(25, 0, 1) % (i + 1);
		printf("%ld\n", sum);
		return 0;
	})M");

	Machine<RISCV64>::setup_minimal_syscalls();

	// The printer is a plain function pointer, so the output has to live outside.
	static std::string g_output;
	auto run = [&binary] (bool background) -> std::string {
		MachineOptions<RISCV64> options;
		options.use_shared_execute_segments = false;
	#ifdef RISCV_BINARY_TRANSLATION
		options.translate_enabled = false;
	#endif
		options.asmjit_enabled = true;
		if (background)
			options.asmjit_background_callback = background_callback;

		g_output.clear();
		Machine<RISCV64> machine { binary, options };
		// A static binary runs its own libc startup, so it needs a real Linux
		// environment and syscall layer to reach main() at all.
		machine.setup_linux({"bgcompile"}, {"LC_ALL=C"});
		machine.setup_linux_syscalls();
		machine.set_printer([] (const Machine<RISCV64>&, const char* p, size_t len) {
			g_output.append(p, len);
		});
		machine.simulate(20'000'000'000ULL);
		REQUIRE(machine.return_value<int>() == 0);
		return g_output;
	};

	const std::string interpreted = run(false);
	REQUIRE(!interpreted.empty());
	// The same program, translated on another thread and live-patched into the
	// running execution, must produce exactly the same output.
	REQUIRE(run(true) == interpreted);

	wait_for_background_threads();
	REQUIRE(g_bg_threads.load() == 0);
}
#endif // RISCV_ASMJIT
