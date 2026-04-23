#include "script.hpp"
#include <libriscv/guest_datatypes.hpp>
#include <algorithm>
#include <numeric>
#include <time.h>
#include <vector>

using GuestString = riscv::GuestStdString<Script::MARCH>;
template <typename T>
using GuestVector = riscv::GuestStdVector<Script::MARCH, T>;
using ScopedString = riscv::ScopedArenaObject<Script::MARCH, GuestString>;
template <typename T>
using ScopedVector = riscv::ScopedArenaObject<Script::MARCH, GuestVector<T>>;
template <typename T>
using ScopedArenaObject = riscv::ScopedArenaObject<Script::MARCH, T>;

int main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <guest.elf>\n", argv[0]);
		return 1;
	}

	printf("=== Creating Script (boots the guest) ===\n");
	Script script("test", argv[1]);

	// --- Two-Phase Host Function Resolution ---
	printf("\n=== Calling on_init (init-only functions available) ===\n");
	Event<void()> on_init(script, "on_init");
	if (!on_init()) {
		fprintf(stderr, "FAIL: on_init() did not return\n");
		return 1;
	}

	printf("\n=== Phase 2: Resolve host functions (initialization=false) ===\n");
	script.resolve_host_functions(/*initialization=*/false);

	// --- Test: Generated host functions ---
	printf("\n=== Test: Generated host functions (Math, IO, Game::get_time) ===\n");
	Event<int()> test_math(script, "test_math");
	if (auto ret = test_math())
		printf("Host got test_math result: %d (expected 84)\n", *ret);
	else {
		fprintf(stderr, "FAIL: test_math() did not return\n");
		return 1;
	}

	Event<void()> test_io(script, "test_io");
	if (!test_io()) {
		fprintf(stderr, "FAIL: test_io() did not return\n");
		return 1;
	}

	Event<double()> test_get_time(script, "test_get_time");
	if (auto ret = test_get_time())
		printf("Host got time: (returned successfully)\n");
	else {
		fprintf(stderr, "FAIL: test_get_time() did not return\n");
		return 1;
	}

	// --- Test: init-only function should fail at runtime ---
	printf("\n=== Test: Init-only function should fail at runtime ===\n");
	Event<void()> test_init_only(script, "test_init_only_at_runtime");
	if (test_init_only()) {
		fprintf(stderr, "FAIL: init-only function should have thrown!\n");
		return 1;
	}
	printf("OK: init-only function correctly rejected at runtime\n");

	// --- Test: Simple call with integer args ---
	printf("\n=== Test: call compute(17, 25) ===\n");
	Event<int(int, int)> compute(script, "compute");
	if (auto ret = compute(17, 25))
		printf("Host got result: %d\n", *ret);
	else {
		fprintf(stderr, "FAIL: compute() did not return\n");
		return 1;
	}

	// --- Test: Pass string as const char* ---
	printf("\n=== Test: call greet(const char*) ===\n");
	Event<void(std::string)> greet(script, "greet");
	if (!greet("World")) {
		fprintf(stderr, "FAIL: greet() did not return\n");
		return 1;
	}

	// --- Test: Pass std::string& via ScopedArenaObject ---
	printf("\n=== Test: call greet_string(const std::string&) via ScopedArenaObject ===\n");
	{
		ScopedString str(script.machine(), "Arena World");
		Event<void(ScopedString&)> greet_string(script, "greet_string");
		if (!greet_string(str)) {
			fprintf(stderr, "FAIL: greet_string() did not return\n");
			return 1;
		}
	}

	// --- Test: Host-allocate on guest heap, pass data ---
	printf("\n=== Test: host-allocate vector on guest heap, call sum_vector ===\n");
	{
		std::vector<int> data = {100, 200, 300, 400, 500};
		// Create a C++ vector on the guest heap, constructed with test data
		ScopedVector<int> guest_vec(script.machine(), data);

		Event<int(ScopedVector<int>&)> sum_vector(script, "sum_vector");
		if (auto ret = sum_vector(guest_vec)) {
			printf("Host got sum: %d\n", *ret);
			const int total = std::accumulate(data.begin(), data.end(), 0);
			if (*ret != total) {
				fprintf(stderr, "FAIL: sum_vector returned wrong result, expected %d\n", total);
				return 1;
			}
		} else {
			fprintf(stderr, "FAIL: sum_vector() did not return\n");
			return 1;
		}
	}

	// --- Test: Guest ownership of host-allocated memory ---
	printf("\n=== Test: Guest takes ownership of host-allocated string ===\n");
	{
		ScopedString str(script.machine(), "This string is heap-allocated and guest-owned!");
		Event<void(ScopedString&)> take_string(script, "take_string");
		if (!take_string(str)) {
			fprintf(stderr, "FAIL: take_string() did not return\n");
			return 1;
		}
		if (!str->empty()) {
			fprintf(stderr, "FAIL: string should be empty after guest moved from it\n");
			return 1;
		}
		Event<void()> print_stored(script, "print_stored");
		if (!print_stored()) {
			fprintf(stderr, "FAIL: print_stored() did not return\n");
			return 1;
		}
	}

	// --- Test: Local lambda callback via generated host function ---
	printf("\n=== Test: Local lambda callback with capture storage ===\n");
	{
		Event<int()> test_cb(script, "test_local_callback");
		if (auto ret = test_cb())
			printf("Host got callback result: %d (expected 720)\n", *ret);
		else {
			fprintf(stderr, "FAIL: test_local_callback() did not return\n");
			return 1;
		}
	}

	// --- Test: RPC between same-program instances ---
	printf("\n=== Test: RPC between two VMs running the same binary ===\n");
	{
		Script script_b("script_b", argv[1]);
		script_b.resolve_host_functions(/*initialization=*/false);

		script.set_peer(&script_b);
		script_b.set_peer(&script);

		Event<int()> get_counter_a(script, "get_shared_counter");
		Event<int()> get_counter_b(script_b, "get_shared_counter");
		printf("Before RPC: script_a counter=%d, script_b counter=%d\n",
			*get_counter_a(), *get_counter_b());

		printf("script_a invoking lambda on script_b...\n");
		Event<int()> rpc_test(script, "test_rpc_invoke");
		if (!rpc_test.is_callable()) {
			fprintf(stderr, "FAIL: test_rpc_invoke not found\n");
			return 1;
		}
		rpc_test();

		int counter_a = *get_counter_a();
		int counter_b = *get_counter_b();
		printf("After RPC: script_a counter=%d, script_b counter=%d\n",
			counter_a, counter_b);

		if (counter_a != 0) {
			fprintf(stderr, "FAIL: script_a counter should be 0, got %d\n", counter_a);
			return 1;
		}
		if (counter_b != 10) {
			fprintf(stderr, "FAIL: script_b counter should be 10, got %d\n", counter_b);
			return 1;
		}
		printf("OK: RPC correctly modified script_b's state without affecting script_a\n");

		script.set_peer(nullptr);
		script_b.set_peer(nullptr);
	}

	// --- Benchmark: vmcall latency ---
	printf("\n=== Benchmark: vmcall latency (10k calls x 100 rounds) ===\n");
	{
		static constexpr int CALLS_PER_ROUND = 10'000;
		static constexpr int ROUNDS = 100;

		Event<void()> increment(script, "increment_counter");
		if (!increment.is_callable()) {
			fprintf(stderr, "FAIL: benchmark function not found\n");
			return 1;
		}

		std::vector<double> round_times_ns(ROUNDS);

		for (int r = 0; r < ROUNDS; r++) {
			struct timespec t0, t1;
			clock_gettime(CLOCK_MONOTONIC, &t0);
			for (int i = 0; i < CALLS_PER_ROUND; i++)
				increment();
			clock_gettime(CLOCK_MONOTONIC, &t1);
			double elapsed_ns = (t1.tv_sec - t0.tv_sec) * 1e9
				+ (t1.tv_nsec - t0.tv_nsec);
			round_times_ns[r] = elapsed_ns / CALLS_PER_ROUND;
		}

		std::sort(round_times_ns.begin(), round_times_ns.end());
		double median_ns = round_times_ns[ROUNDS / 2];
		printf("Per-call latency: median=%.1f ns\n", median_ns);
	}

	printf("\n=== All tests passed ===\n");
	std::string rss = "";
#ifdef __linux__
	// Measure current RSS from /proc/self/statm (Linux-specific)
	FILE* f = fopen("/proc/self/statm", "r");
	if (f) {
		unsigned long size, resident, share, text, lib, data, dt;
		if (fscanf(f, "%lu %lu %lu %lu %lu %lu %lu",
			&size, &resident, &share, &text, &lib, &data, &dt) == 7) {
			const double rss_mb = resident * (sysconf(_SC_PAGESIZE) / (1024.0 * 1024.0));
			char buffer[64];
			snprintf(buffer, sizeof(buffer), "  RSS: %.2f MB", rss_mb);
			rss = buffer;
		}
		fclose(f);
	}
#endif
	const auto current_mmap = script.machine().memory.mmap_address();
	const auto total_memory = Script::MAX_MEMORY;
	printf("Current guest memory/mmap allocated: %.2f MB / %.2f MB%s\n",
		current_mmap / (1024.0 * 1024.0), total_memory / (1024.0 * 1024.0), rss.c_str());
	return 0;
}
