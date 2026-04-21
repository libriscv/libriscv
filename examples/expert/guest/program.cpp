#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>
#include "host_functions.h"

extern "C" __attribute__((noreturn)) void fast_exit(int);

#define PUBLIC(x) extern "C" __attribute__((used, retain)) x

int main()
{
	printf("Guest: Booting up...\n");

	std::string greeting = "Hello from the RISC-V guest!";
	printf("Guest: %s\n", greeting.c_str());

	std::vector<int> numbers = {10, 20, 30, 40, 50};
	int sum = 0;
	for (int n : numbers)
		sum += n;
	printf("Guest: sum of vector = %d\n", sum);

	printf("Guest: init complete, pausing.\n");
	fflush(stdout);
	fast_exit(0);
}

// --- Init-phase function (only callable during initialization) ---
PUBLIC(void on_init())
{
	printf("Guest on_init: calling sys_game_init_world...\n");
	sys_game_init_world("TestWorld");
	printf("Guest on_init: world initialized.\n");
	fflush(stdout);
}

// --- Runtime functions that exercise the generated host functions ---
PUBLIC(int test_math())
{
	int sum = sys_math_add(17, 25);
	printf("Guest: sys_math_add(17, 25) = %d\n", sum);

	int product = sys_math_multiply(6, 7);
	printf("Guest: sys_math_multiply(6, 7) = %d\n", product);

	fflush(stdout);
	return sum + product;
}

PUBLIC(void test_io())
{
	sys_io_print("Hello from guest via IO::print!");
	fflush(stdout);
}

PUBLIC(double test_get_time())
{
	double t = sys_game_get_time();
	printf("Guest: sys_game_get_time() = %f\n", t);
	fflush(stdout);
	return t;
}

PUBLIC(void test_init_only_at_runtime())
{
	printf("Guest: attempting to call init-only function at runtime...\n");
	fflush(stdout);
	sys_game_init_world("ShouldFail");
}

// --- Direct call tests ---
PUBLIC(int compute(int a, int b))
{
	printf("Guest compute(%d, %d) = %d\n", a, b, a + b);
	fflush(stdout);
	return a + b;
}

PUBLIC(void greet(const char* name))
{
	printf("Guest greet: Hello, %s! (via const char*)\n", name);
	fflush(stdout);
}

PUBLIC(void greet_string(const std::string& name))
{
	std::string result = "Hello, " + name + "! (via std::string&)";
	printf("Guest greet_string: %s\n", result.c_str());
	fflush(stdout);
}

static std::string stored_string;

PUBLIC(void take_string(std::string& s))
{
	printf("Guest take_string: received '%s' (len=%zu)\n", s.c_str(), s.size());
	stored_string = std::move(s);
	printf("Guest take_string: moved into static, source now empty=%d\n", s.empty());
	fflush(stdout);
}

PUBLIC(void print_stored())
{
	printf("Guest print_stored: '%s'\n", stored_string.c_str());
	fflush(stdout);
}

PUBLIC(int sum_array(const int* data, int count))
{
	int total = 0;
	for (int i = 0; i < count; i++)
		total += data[i];
	printf("Guest sum_array(%d elements) = %d\n", count, total);
	fflush(stdout);
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
		printf("  Guest callback: captured values = %d, %d, %d, %d\n", value1, value2, value3, value4);
	});
	printf("Guest: local callback test complete\n");
	fflush(stdout);
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
			fprintf(stderr, "FAIL: RPC callback captured wrong float values: x=%f, y=%f\n", x, y);
			exit(1);
		}
		shared_counter += delta;
		printf("  Guest RPC target: shared_counter += %d, now = %d\n", delta, shared_counter);
		fflush(stdout);
	});
	printf("Guest: RPC invoke complete\n");
	fflush(stdout);
	return 0;
}

PUBLIC(int get_shared_counter())
{
	return shared_counter;
}
