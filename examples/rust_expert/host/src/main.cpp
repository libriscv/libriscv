/**
 * The test harness. Every section is one way a Rust guest and a C++ host talk
 * to each other, and they build on one another in order:
 *
 *   1. Generated host functions, resolved by hash at load time
 *   2. Two-phase init, with init-only functions locked out afterwards
 *   3. Plain vmcalls with integers
 *   4. Rust values crossing the boundary in both directions
 *   5. Ownership moving across, in both directions
 *   6. Closures called back through the host, locally and into a second VM
 *   7. What a vmcall costs
 */
#include "script.hpp"
#include <libriscv/guest/guest_rust_string.hpp>
#include <libriscv/guest/guest_rust_vec.hpp>
#include <algorithm>
#include <numeric>
#include <time.h>
#include <vector>

using RustString = riscv::GuestRustString<Script::MARCH>;
template <typename T>
using RustVec = riscv::GuestRustVec<Script::MARCH, T>;
using ScopedRustString = riscv::ScopedGuestRustString<Script::MARCH>;
template <typename T>
using ScopedRustVec = riscv::ScopedGuestRustVec<Script::MARCH, T>;

/// A block of raw bytes in the shared arena. A Rust &str is not
/// zero-terminated, so a borrowed string is handed over as address + length.
struct GuestBytes {
	GuestBytes(Script& script, std::string_view bytes)
		: m_script(script), m_len(bytes.size())
	{
		m_addr = script.guest_alloc(bytes.size());
		if (m_addr == 0x0)
			throw std::runtime_error("Out of guest arena memory");
		script.machine().copy_to_guest(m_addr, bytes.data(), bytes.size());
	}
	~GuestBytes() { m_script.guest_free(m_addr); }

	Script::gaddr_t address() const noexcept { return m_addr; }
	Script::gaddr_t size() const noexcept { return m_len; }

	Script& m_script;
	Script::gaddr_t m_addr;
	Script::gaddr_t m_len;
};

int main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <guest.elf>\n", argv[0]);
		return 1;
	}

	printf("=== Creating Script (boots the Rust guest) ===\n");
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
	printf("\n=== Test: Generated host functions (math, io, game) ===\n");
	Event<int()> test_math(script, "test_math");
	if (auto ret = test_math()) {
		printf("Host got test_math result: %d (expected 84)\n", *ret);
		if (*ret != 84) {
			fprintf(stderr, "FAIL: test_math returned %d\n", *ret);
			return 1;
		}
	} else {
		fprintf(stderr, "FAIL: test_math() did not return\n");
		return 1;
	}

	Event<void()> test_io(script, "test_io");
	if (!test_io()) {
		fprintf(stderr, "FAIL: test_io() did not return\n");
		return 1;
	}

	Event<double()> test_get_time(script, "test_get_time");
	if (!test_get_time()) {
		fprintf(stderr, "FAIL: test_get_time() did not return\n");
		return 1;
	}
	printf("Host got time: (returned successfully)\n");

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

	// --- Test: Pass a borrowed &str (address and length in two registers) ---
	printf("\n=== Test: call greet(&str) ===\n");
	{
		GuestBytes name(script, "World");
		if (!script.call("greet", name.address(), name.size())) {
			fprintf(stderr, "FAIL: greet() did not return\n");
			return 1;
		}
	}

	// --- Test: Pass a &String the host built in the shared arena ---
	printf("\n=== Test: call greet_string(&String) ===\n");
	{
		ScopedRustString str(script.machine(), "Arena World");
		Event<void(ScopedRustString&)> greet_string(script, "greet_string");
		if (!greet_string(str)) {
			fprintf(stderr, "FAIL: greet_string() did not return\n");
			return 1;
		}
	}

	// --- Test: Host-build a Vec<i32> on the guest heap ---
	printf("\n=== Test: host-allocate Vec<i32> in the arena, call sum_vector ===\n");
	{
		std::vector<int32_t> data = {100, 200, 300, 400, 500};
		ScopedRustVec<int32_t> guest_vec(script.machine(), data);

		Event<int(ScopedRustVec<int32_t>&)> sum_vector(script, "sum_vector");
		if (auto ret = sum_vector(guest_vec)) {
			printf("Host got sum: %d\n", *ret);
			const int32_t total = std::accumulate(data.begin(), data.end(), 0);
			if (*ret != total) {
				fprintf(stderr, "FAIL: sum_vector returned wrong result, expected %d\n", total);
				return 1;
			}
		} else {
			fprintf(stderr, "FAIL: sum_vector() did not return\n");
			return 1;
		}
	}

	// --- Test: A String the guest returns by value ---
	printf("\n=== Test: guest returns a String by value ===\n");
	{
		// It does not fit in a register, so the host allocates room for it and
		// passes the address as a hidden first argument
		ScopedRustString greeting(script.machine());
		Event<void(ScopedRustString&)> make_greeting(script, "make_greeting");
		if (!make_greeting(greeting)) {
			fprintf(stderr, "FAIL: make_greeting() did not return\n");
			return 1;
		}
		printf("Host got String from guest: '%s' (%zu bytes, capacity %zu)\n",
			greeting->to_string(script.machine()).c_str(),
			greeting->size(), greeting->capacity());
		if (greeting->empty()) {
			fprintf(stderr, "FAIL: guest returned an empty String\n");
			return 1;
		}
		// Allocated by the guest, freed by the host, out of the one arena
	}

	// --- Test: Host functions that fill in guest-owned collections ---
	printf("\n=== Test: guest asks the host to fill a String and a Vec<u32> ===\n");
	{
		Event<int()> test_host_fills(script, "test_host_fills");
		if (auto ret = test_host_fills()) {
			// 1..10 summed, plus the 1000 the guest pushed on the end
			printf("Host got test_host_fills result: %d (expected 1055)\n", *ret);
			if (*ret != 1055) {
				fprintf(stderr, "FAIL: test_host_fills returned %d\n", *ret);
				return 1;
			}
		} else {
			fprintf(stderr, "FAIL: test_host_fills() did not return\n");
			return 1;
		}
	}

	// --- Test: Guest ownership of host-allocated memory ---
	printf("\n=== Test: Guest takes ownership of a host-allocated String ===\n");
	{
		ScopedRustString str(script.machine(),
			"This string is heap-allocated and becomes guest-owned!");
		Event<void(ScopedRustString&)> take_string(script, "take_string");
		if (!take_string(str)) {
			fprintf(stderr, "FAIL: take_string() did not return\n");
			return 1;
		}
		// The guest moved out of it, leaving an empty String behind, so the
		// scoped object below releases nothing
		if (!str->empty() || str->capacity() != 0) {
			fprintf(stderr, "FAIL: String should be empty after the guest moved from it\n");
			return 1;
		}
		Event<void()> print_stored(script, "print_stored");
		if (!print_stored()) {
			fprintf(stderr, "FAIL: print_stored() did not return\n");
			return 1;
		}
		// The guest still owns the host's allocation until it drops it
		Event<void()> release_stored(script, "release_stored");
		release_stored();
	}

	// --- Test: Local closure callback via a generated host function ---
	printf("\n=== Test: Rust closure called back through the host ===\n");
	{
		Event<int()> test_cb(script, "test_local_callback");
		if (auto ret = test_cb()) {
			printf("Host got callback result: %d (expected 720)\n", *ret);
			if (*ret != 720) {
				fprintf(stderr, "FAIL: test_local_callback returned %d\n", *ret);
				return 1;
			}
		} else {
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

		printf("script_a invoking its closure on script_b...\n");
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
		printf("OK: RPC modified script_b's state without touching script_a\n");

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
		printf("Per-call latency: median=%.1f ns\n", round_times_ns[ROUNDS / 2]);
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
