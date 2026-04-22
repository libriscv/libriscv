#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>
#include "host_functions.h"
#include <fmt/core.h>

template <typename... Args>
static inline void print(fmt::format_string<Args...> fmt_str, Args&&... args) {
	char buffer[2048];
	auto result = fmt::format_to_n(buffer, sizeof(buffer) - 1, fmt_str, std::forward<Args>(args)...);
	sys_print(buffer, result.size);
}

int main()
{
	print("Guest: Booting up...");

	std::string greeting = "Hello from the RISC-V guest!";
	print("Guest: {}", greeting);

	std::vector<int> numbers = {10, 20, 30, 40, 50};
	int sum = 0;
	for (int n : numbers)
		sum += n;
	print("Guest: sum of vector = {}", sum);

	print("Guest: init complete, pausing.");
	fast_exit(0);
}

// --- Init-phase function (only callable during initialization) ---
PUBLIC(void on_init())
{
	print("Guest on_init: calling sys_game_init_world...");
	sys_game_init_world("TestWorld");
	print("Guest on_init: world initialized.");
}

// --- Runtime functions that exercise the generated host functions ---
PUBLIC(int test_math())
{
	int sum = sys_math_add(17, 25);
	print("Guest: sys_math_add(17, 25) = {}", sum);

	int product = sys_math_multiply(6, 7);
	print("Guest: sys_math_multiply(6, 7) = {}", product);

	return sum + product;
}

PUBLIC(void test_io())
{
	print("Hello from guest via IO::print!");
}

PUBLIC(double test_get_time())
{
	double t = sys_game_get_time();
	print("Guest: sys_game_get_time() = {:.2f}", t);
	return t;
}

PUBLIC(void test_init_only_at_runtime())
{
	print("Guest: attempting to call init-only function at runtime...");
	sys_game_init_world("ShouldFail");
}

// --- Direct call tests ---
PUBLIC(int compute(int a, int b))
{
	print("Guest compute({}, {}) = {}", a, b, a + b);
	return a + b;
}

PUBLIC(void greet(const char* name))
{
	print("Guest greet: Hello, {}! (via const char*)", name);
}

PUBLIC(void greet_string(const std::string& name))
{
	std::string result = "Hello, " + name + "! (via std::string&)";
	print("Guest greet_string: {}", result);
}

static std::string stored_string;

PUBLIC(void take_string(std::string& s))
{
	print("Guest take_string: received '{}'", s);
	stored_string = std::move(s);
	print("Guest take_string: moved into static, source now empty={}", s.empty());
}

PUBLIC(void print_stored())
{
	print("Guest print_stored: '{}'", stored_string);
}

PUBLIC(int sum_vector(const std::vector<int>& data))
{
	int total = 0;
	for (auto i : data)
		total += i;
	print("Guest sum_vector({} elements) = {}", data.size(), total);
	return total;
}

PUBLIC(void increment_counter())
{
}

PUBLIC(long get_counter())
{
	return 0;
}

// --- RPC support via generated host functions ---

template <typename F>
static void store_and_callback(F callback) {
	static_assert(sizeof(F) <= 24, "Capture too large for storage");
	static_assert(std::is_trivially_copyable_v<F>, "Capture must be trivially copyable");
	sys_rpc_callback(
		+[](void* data) { (*(F*)data)(); },
		(void*)&callback, sizeof(callback));
}

template <typename F>
static long invoke_elsewhere(F callback) {
	static_assert(sizeof(F) <= 24, "Capture too large for storage");
	static_assert(std::is_trivially_copyable_v<F>, "Capture must be trivially copyable");
	return sys_rpc_invoke(
		+[](void* data) { (*(F*)data)(); },
		(void*)&callback, sizeof(callback));
}

PUBLIC(int test_local_callback())
{
	int value1 = 42;
	int value2 = 99;
	int value3 = 123;
	int value4 = 456;
	store_and_callback([value1, value2, value3, value4]() {
		print("  Guest callback: captured values = {}, {}, {}, {}", value1, value2, value3, value4);
	});
	print("Guest: local callback test complete");
	return value1 + value2 + value3 + value4;
}

static int shared_counter = 0;

PUBLIC(int test_rpc_invoke())
{
	int delta = 10;
	float x = 3.14f;
	float y = 2.718f;
	invoke_elsewhere([delta, x, y]() {
		if (x != 3.14f || y != 2.718f) {
			print("FAIL: RPC callback captured wrong float values: x={}, y={}", x, y);
			exit(1);
		}
		shared_counter += delta;
		print("  Guest RPC target: shared_counter += {}, now = {}", delta, shared_counter);
	});
	print("Guest: RPC invoke complete");
	return 0;
}

PUBLIC(int get_shared_counter())
{
	return shared_counter;
}
