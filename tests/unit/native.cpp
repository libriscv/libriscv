#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libriscv/machine.hpp>
#include <libriscv/native_heap.hpp>
extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2 -static", bool cpp = false);
static const uint64_t MAX_INSTRUCTIONS = 10'000'000ul;
static const std::string cwd {SRCDIR};
static bool is_zig() {
	const char* rcc = getenv("RCC");
	if (rcc == nullptr)
		return false;
	return std::string(rcc).find("zig") != std::string::npos;
}
using namespace riscv;

static const int HEAP_SYSCALLS_BASE	  = 470;
static const int MEMORY_SYSCALLS_BASE = 475;
static const int THREADS_SYSCALL_BASE = 490;

using CppString = GuestStdString<RISCV64>;
template <typename T>
using CppVector = GuestStdVector<RISCV64, T>;
template <typename K, typename V>
using CppMap = GuestStdUnorderedMap<RISCV64, K, V>;
using ScopedCppString = ScopedArenaObject<RISCV64, CppString>;
template <typename T>
using ScopedCppVector = ScopedArenaObject<RISCV64, CppVector<T>>;
template <typename K, typename V>
using ScopedCppMap = ScopedArenaObject<RISCV64, CppMap<K, V>>;

template <int W>
static void setup_native_system_calls(riscv::Machine<W>& machine, size_t heap_size = 65536)
{
	// Syscall-backed heap
	auto heap = machine.memory.mmap_allocate(heap_size);

	machine.setup_native_heap(HEAP_SYSCALLS_BASE, heap, heap_size);
	machine.setup_native_memory(MEMORY_SYSCALLS_BASE);
	machine.setup_native_threads(THREADS_SYSCALL_BASE);
}

TEST_CASE("Activate native helper syscalls", "[Native]")
{
	const auto binary = build_and_load(R"M(
	#include <stdlib.h>
	#include <stdio.h>
	int main(int argc, char** argv)
	{
		const char *hello = (const char*)atol(argv[1]);
		printf("%s\n", hello);
		return 666;
	})M");

	riscv::Machine<RISCV64> machine { binary };
	machine.setup_linux_syscalls();

	setup_native_system_calls(machine);

	// Allocate string on heap
	static const std::string hello = "Hello World!";
	auto addr = machine.arena().malloc(64);
	machine.copy_to_guest(addr, hello.data(), hello.size()+1);

	// Pass string address to guest as main argument
	machine.setup_linux(
		{"native", std::to_string(addr)},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	// Catch output from machine
	struct State {
		bool output_is_hello_world = false;
	} state;

	machine.set_userdata(&state);
	machine.set_printer([] (const auto& m, const char* data, size_t size) {
		auto* state = m.template get_userdata<State> ();
		std::string text{data, data + size};
		// musl writev:
		state->output_is_hello_world = state->output_is_hello_world || (text == "Hello World!");
		// glibc write:
		state->output_is_hello_world = state->output_is_hello_world || (text == "Hello World!\n");
	});

	// Run simulation
	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(machine.return_value() == 666);
	REQUIRE(state.output_is_hello_world);
}

TEST_CASE("Use native helper syscalls", "[Native]")
{
	const auto binary = build_and_load(R"M(
	#include <include/native_libc.h>
	#include <stdlib.h>
	#include <stdio.h>
	int main()
	{
		char* hello = malloc(13);
		memcpy(hello, "Hello World!", 13);
		hello = realloc(hello, 128);
		printf("%s\n", hello);
		free(hello);
		return 666;
	})M", "-O2 -static -I" + cwd);

	riscv::Machine<RISCV64> machine { binary };

	setup_native_system_calls(machine);

	machine.setup_linux_syscalls();
	machine.setup_linux(
		{"native"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	// Catch output from machine
	struct State {
		bool output_is_hello_world = false;
	} state;

	machine.set_userdata(&state);
	machine.set_printer([] (const auto& m, const char* data, size_t size) {
		auto* state = m.template get_userdata<State> ();
		std::string text{data, data + size};
		// musl writev:
		state->output_is_hello_world = state->output_is_hello_world || (text == "Hello World!");
		// glibc write:
		state->output_is_hello_world = state->output_is_hello_world || (text == "Hello World!\n");
	});

	// Run simulation
	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(machine.return_value() == 666);
	REQUIRE(state.output_is_hello_world);
}

TEST_CASE("Free unknown causes exception", "[Native]")
{
	const auto binary = build_and_load(R"M(
	#include <include/native_libc.h>
	int main()
	{
		free((void *)0x1234);
		return 666;
	})M", "-O2 -static -I" + cwd);

	riscv::Machine<RISCV64> machine { binary };
	setup_native_system_calls(machine);

	machine.setup_linux_syscalls();
	machine.setup_linux(
		{"native"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	bool error = false;
	try {
		machine.simulate(MAX_INSTRUCTIONS);
	} catch (const std::exception& e) {
		// Libtcc does not forward the real exception (instead throws a generic SYSTEM_CALL_FAILED)
		if constexpr (!libtcc_enabled)
			REQUIRE(std::string(e.what()) == "Possible double-free for freed pointer");
		error = true;
	}
	REQUIRE(error);
}

TEST_CASE("VM calls with std::string and std::vector", "[Native]")
{
	if (is_zig()) // We don't support libc++ std::string yet
		return;

	const auto binary = build_and_load(R"M(
	#include <string>
	#include <vector>
	#include <cassert>

	void* operator new(size_t size) {
		return malloc(size);
	}
	void operator delete(void* ptr) {
		free(ptr);
	}

	extern "C" __attribute__((used, retain))
	void test(std::string& str,
		const std::vector<int>& ints,
		const std::vector<std::string>& strings)
	{
		std::string result = "Hello, " + str + "! Integers:";
		for (auto i : ints)
			result += " " + std::to_string(i);
		result += " Strings:";
		for (const auto& s : strings)
			result += " " + s;
		str = result;
	}

	struct Data {
		int a, b, c, d;
	};

	extern "C" __attribute__((used, retain))
	void test2(Data* data) {
		assert(data->a == 1);
		assert(data->b == 2);
		assert(data->c == 3);
		assert(data->d == 4);
		data->a = 5;
		data->b = 6;
		data->c = 7;
		data->d = 8;
	}

	extern "C" __attribute__((used, retain))
	int test3(std::vector<std::vector<int>>& vec) {
		assert(vec.size() == 2);
		assert(vec[0].size() == 3);
		assert(vec[1].size() == 2);
		assert(vec[0][0] == 1);
		assert(vec[0][1] == 2);
		assert(vec[0][2] == 3);
		assert(vec[1][0] == 4);
		assert(vec[1][1] == 5);

		vec.at(1).push_back(666);
		return 666;
	}

	int main() {
		return 666;
	})M", "-O2 -static -x c " + cwd + "/include/native_libc.h -x c++ ", true);

	riscv::Machine<RISCV64> machine { binary };
	setup_native_system_calls(machine);
	machine.setup_linux_syscalls();
	machine.setup_linux(
		{"vmcall"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	machine.simulate(MAX_INSTRUCTIONS);
	REQUIRE(machine.return_value<int>() == 666);

	// Define the test6 function, which has a std::string& argument in the guest,
	// and a std::vector<int>& and std::vector<std::string>&. The stack is writable,
	// so the guest can choose whether or not to use const references.
	const unsigned allocs_before = machine.arena().allocation_counter() - machine.arena().deallocation_counter();

	for (int i = 0; i < 10; i++) {
		// Create a GuestStdString object with a string
		ScopedCppString str(machine);
		REQUIRE(str->empty());
		str = "C++ World ..SSO..";
		REQUIRE(str->to_string(machine) == "C++ World ..SSO..");

		// Create a GuestStdVector object with a vector of integers
		ScopedCppVector<int> ivec(machine);
		REQUIRE(ivec->empty());
		ivec = std::vector<int>{ 1, 2, 3 };
		REQUIRE(ivec->size() == 3);
		ivec->assign(machine, std::vector<int>{ 1, 2, 3, 4, 5 });
		REQUIRE(ivec->size() == 5);

		// Create a vector of strings using a specialization for std::string
		ScopedCppVector<CppString> svec(machine,
			std::vector<std::string>{ "Hello,", "World!", "This string is long :)" });
		REQUIRE(svec->size() == 3);

		machine.vmcall("test", str, ivec, svec);

		// Check that the string was modified
		REQUIRE(str->to_string(machine) == "Hello, C++ World ..SSO..! Integers: 1 2 3 4 5 Strings: Hello, World! This string is long :)");
	}

	// Check that the number of active allocations is the same as before the test
	const unsigned allocs_now = machine.arena().allocation_counter() -
		machine.arena().deallocation_counter();
	REQUIRE(allocs_now == allocs_before);

	// Test the second function
	for (int i = 0; i < 10; i++) {
		// Scoped arena objects are guest-heap allocated, which means we can read back data
		// from the guest after the function call
		struct Data {
			int a, b, c, d;
		};
		ScopedArenaObject<RISCV64, Data> data(machine, Data{1, 2, 3, 4});

		machine.vmcall("test2", data);

		// Check that the struct was modified
		REQUIRE(data->a == 5);
		REQUIRE(data->b == 6);
		REQUIRE(data->c == 7);
		REQUIRE(data->d == 8);
	}

	const unsigned allocs_after2 = machine.arena().allocation_counter() -
		machine.arena().deallocation_counter();
	REQUIRE(allocs_after2 == allocs_before);

	// Test the third function
	for (int i = 0; i < 10; i++) {
		ScopedCppVector<CppVector<int>> vec(machine);
		vec->push_back(machine, std::vector<int>{1, 2, 3});
		vec->push_back(machine, std::vector<int>{4, 5});
		REQUIRE(vec->size() == 2);
		REQUIRE(vec->capacity() >= 2);
		vec->clear(machine);
		REQUIRE(vec->empty());
		REQUIRE(vec->capacity() >= 2);
		vec->push_back(machine, std::vector<int>{1, 2, 3});
		vec->push_back(machine, std::vector<int>{4, 5});
		REQUIRE(vec->size() == 2);
		// Using reserve increases the capacity, but not the size
		vec->reserve(machine, 16);
		REQUIRE(vec->capacity() >= 16);
		REQUIRE(vec->size() == 2);
		// Check that the vectors were correctly initialized
		REQUIRE(vec->at(machine, 0).size() == 3);
		REQUIRE(vec->at(machine, 1).size() == 2);
		REQUIRE(vec->at(machine, 0).at(machine, 0) == 1);
		REQUIRE(vec->at(machine, 0).at(machine, 1) == 2);
		REQUIRE(vec->at(machine, 0).at(machine, 2) == 3);
		REQUIRE(vec->at(machine, 1).at(machine, 0) == 4);
		REQUIRE(vec->at(machine, 1).at(machine, 1) == 5);

		const int ret = machine.vmcall("test3", vec);

		// Check that the function returned the expected value
		REQUIRE(ret == 666);
		REQUIRE(vec->size() == 2);
		// We modified the second vector, adding an element
		REQUIRE(vec->at(machine, 1).size() == 3);
		REQUIRE(vec->at(machine, 1).at(machine, 2) == 666);

		// Test iterators (slightly more complex)
		size_t count = 0;
		auto begin = vec->begin(machine);
		auto end = vec->end(machine);
		for (auto it = begin; it != end; ++it) {
			auto& v = *it;
			for (size_t i = 0; i < v.size(); i++) {
				count += v.at(machine, i);
			}
		}
		REQUIRE(count == 1 + 2 + 3 + 4 + 5 + 666);
	}

	const unsigned allocs_after3 = machine.arena().allocation_counter() -
		machine.arena().deallocation_counter();
	REQUIRE(allocs_after3 == allocs_before);
}

TEST_CASE("Guest std::hash matches libstdc++", "[Native]")
{
	// std::hash<std::string> is a MurmurHash2 variant, and the 64-bit variant
	// can be verified directly against the hosts libstdc++
	const std::vector<std::string> strings {
		"", "a", "ab", "abc", "abcd", "Hello", "Hello, World!",
		"0123456789abcdefghij", std::string(1000, 'x')
	};
	for (const auto& str : strings) {
		REQUIRE(GuestStdHashBytes<RISCV64>::hash(str.data(), str.size())
			== std::hash<std::string>{}(str));
	}

	// The 32-bit variant is verified against reference values produced by a
	// 32-bit RISC-V guest using libstdc++ std::hash<std::string>
	const struct { const char* str; uint32_t hash; } references[] = {
		{"", 3990065800u},
		{"a", 2167009006u},
		{"ab", 2805137849u},
		{"abc", 3350977461u},
		{"abcd", 804720481u},
		{"Hello", 101669370u},
		{"Hello, World!", 192903281u},
		{"0123456789abcdefghij", 3446580841u},
	};
	for (const auto& ref : references) {
		REQUIRE(GuestStdHashBytes<RISCV32>::hash(ref.str, strlen(ref.str)) == ref.hash);
	}

	// std::hash of a floating-point value hashes the bytes, except zeroes
	const double value = 3.5;
	REQUIRE(GuestStdHashBytes<RISCV64>::hash(&value, sizeof(value))
		== std::hash<double>{}(value));
	REQUIRE(GuestStdHashBytes<RISCV32>::hash(&value, sizeof(value)) == 3508813564u);

	// The nodes of a map with a std::string key store the hash code, and
	// the nodes of a map with an integer key do not
	static_assert(CppMap<CppString, int>::cache_hash_code == true);
	static_assert(CppMap<long, long>::cache_hash_code == false);
	static_assert(sizeof(CppMap<CppString, int>::node_type) == 56);
	static_assert(sizeof(CppMap<CppString, CppString>::node_type) == 80);
	static_assert(sizeof(CppMap<long, long>::node_type) == 24);
}

TEST_CASE("VM calls with std::unordered_map", "[Native]")
{
	if (is_zig()) // We don't support libc++ std::string yet
		return;

	const auto binary = build_and_load(R"M(
	#include <string>
	#include <unordered_map>
	#include <cassert>

	void* operator new(size_t size) {
		return malloc(size);
	}
	void operator delete(void* ptr) {
		free(ptr);
	}

	// Verify that every element sits in the bucket that its hash says it
	// should be in, and that every element is reachable from the buckets.
	template <typename Map>
	static long check_buckets(Map& map) {
		size_t counted = 0;
		for (size_t b = 0; b < map.bucket_count(); b++) {
			for (auto it = map.begin(b); it != map.end(b); ++it) {
				if (map.bucket(it->first) != b)
					return -1;
				counted += 1;
			}
			if (map.bucket_size(b) != (size_t)std::distance(map.begin(b), map.end(b)))
				return -2;
		}
		if (counted != map.size())
			return -3;
		// The whole element list must be reachable from begin() as well
		if ((size_t)std::distance(map.begin(), map.end()) != map.size())
			return -4;
		return (long)map.size();
	}

	extern "C" __attribute__((used, retain))
	long test_smap(std::unordered_map<std::string, int>& map) {
		if (check_buckets(map) != 3)
			return -10;
		assert(map.size() == 3);
		assert(map.at("one") == 1);
		assert(map.at("two") == 2);
		assert(map.at("three and a long key that is not SSO") == 3);
		assert(map.count("four") == 0);
		assert(map.find("four") == map.end());

		long sum = 0;
		for (const auto& entry : map)
			sum += entry.second + entry.first.size();

		// Modify the map, which the host verifies afterwards
		map["four"] = 4;
		map.erase("one");
		assert(map.size() == 3);
		if (check_buckets(map) != 3)
			return -11;
		return sum;
	}

	extern "C" __attribute__((used, retain))
	long test_ssmap(std::unordered_map<std::string, std::string>& map) {
		if (check_buckets(map) != 2)
			return -10;
		assert(map.at("hello") == "world");
		assert(map.at("a long key that is not SSO") == "a long value that is not SSO");
		map["extra"] = "a long value that will be allocated";
		return check_buckets(map);
	}

	extern "C" __attribute__((used, retain))
	long test_imap(std::unordered_map<long, long>& map) {
		const long buckets = check_buckets(map);
		if (buckets != 100)
			return -10;
		long sum = 0;
		for (long i = 0; i < 100; i++) {
			auto it = map.find(i);
			if (it == map.end() || it->second != i * 10)
				return -11;
			sum += it->second;
		}
		// Erase every third element, and add one new element
		for (long i = 0; i < 100; i += 3)
			map.erase(i);
		map[1000] = 10000;
		if (check_buckets(map) != (long)map.size())
			return -12;
		return sum;
	}

	// Insert many elements into a map created by the host, which forces
	// the guest to rehash a bucket array that the host allocated.
	extern "C" __attribute__((used, retain))
	long test_grow(std::unordered_map<std::string, int>& map) {
		for (int i = 0; i < 200; i++)
			map[std::to_string(i)] = i;
		for (int i = 0; i < 200; i++) {
			if (map.at(std::to_string(i)) != i)
				return -1;
		}
		return check_buckets(map);
	}

	// A map that is built entirely by the guest, and read by the host
	extern "C" __attribute__((used, retain))
	long build_map(std::unordered_map<std::string, std::string>& map) {
		for (int i = 0; i < 64; i++)
			map.emplace("key" + std::to_string(i), "value" + std::to_string(i * 3));
		return check_buckets(map);
	}

	int main() {
		return 666;
	})M", "-O2 -static -x c " + cwd + "/include/native_libc.h -x c++ ", true);

	riscv::Machine<RISCV64> machine { binary };
	// The maps in this test need a larger heap than the default
	setup_native_system_calls(machine, 4UL << 20);
	machine.setup_linux_syscalls();
	machine.setup_linux(
		{"vmcall"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	machine.simulate(MAX_INSTRUCTIONS);
	REQUIRE(machine.return_value<int>() == 666);

	const unsigned allocs_before = machine.arena().allocation_counter()
		- machine.arena().deallocation_counter();

	// std::unordered_map<std::string, int>
	for (int i = 0; i < 10; i++) {
		ScopedCppMap<CppString, int> map(machine);
		REQUIRE(map->empty());
		REQUIRE(map->bucket_count() == 1);

		map->insert_or_assign(machine, map.address(), "one", 1);
		map->insert_or_assign(machine, map.address(), std::string("two"), 2);
		map->insert_or_assign(machine, map.address(), "three and a long key that is not SSO", 3);
		REQUIRE(map->size() == 3);
		REQUIRE(*map->find(machine, "one") == 1);
		REQUIRE(map->at(machine, "two") == 2);
		REQUIRE(map->contains(machine, "three and a long key that is not SSO"));
		REQUIRE(!map->contains(machine, "four"));
		REQUIRE(map->find(machine, "four") == nullptr);

		// Overwriting an existing key does not add an element
		map->insert_or_assign(machine, map.address(), "two", 22);
		REQUIRE(map->size() == 3);
		REQUIRE(map->at(machine, "two") == 22);
		map->insert_or_assign(machine, map.address(), "two", 2);

		// try_emplace() keeps the value of an existing key
		map->try_emplace(machine, map.address(), "two", 222);
		REQUIRE(map->size() == 3);
		REQUIRE(map->at(machine, "two") == 2);
		map->try_emplace(machine, map.address(), "temporary", 5);
		REQUIRE(map->size() == 4);
		REQUIRE(map->erase(machine, map.address(), "temporary"));
		REQUIRE(map->size() == 3);
		REQUIRE(map->max_load_factor() == 1.0f);

		const int64_t sum = machine.vmcall<MAX_INSTRUCTIONS>("test_smap", map);
		// (1 + 3) + (2 + 3) + (3 + 36)
		REQUIRE(sum == 4 + 5 + 39);

		// The guest added "four" and erased "one"
		REQUIRE(map->size() == 3);
		REQUIRE(!map->contains(machine, "one"));
		REQUIRE(map->at(machine, "four") == 4);

		auto host_map = map->to_map(machine);
		REQUIRE(host_map.size() == 3);
		REQUIRE(host_map.at("two") == 2);
		REQUIRE(host_map.at("three and a long key that is not SSO") == 3);
		REQUIRE(host_map.at("four") == 4);
		REQUIRE(host_map.count("one") == 0);
	}

	REQUIRE(machine.arena().allocation_counter()
		- machine.arena().deallocation_counter() == allocs_before);

	// std::unordered_map<std::string, std::string>
	{
		ScopedCppMap<CppString, CppString> map(machine,
			std::unordered_map<std::string, std::string> {
				{"hello", "world"},
				{"a long key that is not SSO", "a long value that is not SSO"}
			});
		REQUIRE(map->size() == 2);
		REQUIRE(map->at(machine, "hello").to_string(machine) == "world");

		REQUIRE(int64_t(machine.vmcall<MAX_INSTRUCTIONS>("test_ssmap", map)) == 3);
		REQUIRE(map->size() == 3);
		REQUIRE(map->at(machine, "extra").to_string(machine)
			== "a long value that will be allocated");

		const auto host_map = map->to_map(machine);
		REQUIRE(host_map.size() == 3);
		REQUIRE(host_map.at("a long key that is not SSO") == "a long value that is not SSO");

		// Assigning a new host map frees the old keys and values
		map = std::unordered_map<std::string, std::string> { {"only", "one"} };
		REQUIRE(map->size() == 1);
		REQUIRE(map->at(machine, "only").to_string(machine) == "one");
	}

	REQUIRE(machine.arena().allocation_counter()
		- machine.arena().deallocation_counter() == allocs_before);

	// std::unordered_map<long, long>, which does not store hash codes
	{
		ScopedCppMap<long, long> map(machine);
		std::unordered_map<long, long> host_map;
		// The bucket growth must match libstdc++ exactly, or the guest will
		// compute other bucket indices than we do
		for (long i = 0; i < 300; i++) {
			map->insert_or_assign(machine, map.address(), i, i * 10);
			host_map.emplace(i, i * 10);
			REQUIRE(map->size() == host_map.size());
			REQUIRE(map->bucket_count() == host_map.bucket_count());
		}
		map->reserve(machine, map.address(), 1000);
		host_map.reserve(1000);
		REQUIRE(map->bucket_count() == host_map.bucket_count());
		map->rehash(machine, map.address(), 4096);
		host_map.rehash(4096);
		REQUIRE(map->bucket_count() == host_map.bucket_count());
		REQUIRE(map->to_map(machine) == host_map);

		// Erasing elements host-side
		for (long i = 0; i < 300; i++) {
			if (i % 2 == 0) {
				REQUIRE(map->erase(machine, map.address(), i));
				REQUIRE(!map->erase(machine, map.address(), i));
				host_map.erase(i);
			}
		}
		REQUIRE(map->size() == 150);
		REQUIRE(map->to_map(machine) == host_map);

		map->clear(machine);
		REQUIRE(map->empty());
		REQUIRE(map->to_map(machine).empty());

		for (long i = 0; i < 100; i++)
			map->insert_or_assign(machine, map.address(), i, i * 10);
		// 0 + 10 + ... + 990
		REQUIRE(int64_t(machine.vmcall<MAX_INSTRUCTIONS>("test_imap", map)) == 49500);
		// The guest erased 34 elements and added one
		REQUIRE(map->size() == 100 - 34 + 1);
		REQUIRE(map->at(machine, 1000) == 10000);
		REQUIRE(!map->contains(machine, 0));
		REQUIRE(!map->contains(machine, 99));
		REQUIRE(map->at(machine, 1) == 10);
	}

	REQUIRE(machine.arena().allocation_counter()
		- machine.arena().deallocation_counter() == allocs_before);

	// The guest inserts 200 elements into a host-created map
	{
		ScopedCppMap<CppString, int> map(machine);
		for (int i = 0; i < 50; i++)
			map->insert_or_assign(machine, map.address(), std::to_string(i), i);
		REQUIRE(map->size() == 50);

		REQUIRE(int64_t(machine.vmcall<MAX_INSTRUCTIONS>("test_grow", map)) == 200);
		REQUIRE(map->size() == 200);

		const auto host_map = map->to_map(machine);
		REQUIRE(host_map.size() == 200);
		for (int i = 0; i < 200; i++)
			REQUIRE(host_map.at(std::to_string(i)) == i);
	}

	// Nested containers: the maps point back into themselves, and they must
	// survive being moved around by the vector
	{
		ScopedCppVector<CppMap<CppString, int>> vec(machine);
		for (int i = 0; i < 8; i++) {
			// push_back() tells the map where in the vector it ended up
			vec->push_back(machine, CppMap<CppString, int>{});
			const auto address = vec->address_at(i);
			vec->at(machine, i).insert_or_assign(machine, address, "index", i);
			vec->at(machine, i).insert_or_assign(machine, address, "double", i * 2);
			// Growing the vector relocates every map in it
			vec->reserve(machine, 16 + i);
			for (int j = 0; j <= i; j++) {
				auto& moved = vec->at(machine, j);
				REQUIRE(moved.size() == 2);
				REQUIRE(moved.at(machine, "index") == j);
				REQUIRE(moved.at(machine, "double") == j * 2);
			}
		}
		REQUIRE(vec->size() == 8);
	}

	REQUIRE(machine.arena().allocation_counter()
		- machine.arena().deallocation_counter() == allocs_before);

	// A map inside a map, freed recursively
	{
		ScopedCppMap<CppString, CppMap<CppString, int>> map(machine);
		map->insert_or_assign(machine, map.address(), "inner",
			std::unordered_map<std::string, int> { {"x", 1}, {"y", 2} });
		auto& inner = map->at(machine, "inner");
		REQUIRE(inner.size() == 2);
		REQUIRE(inner.at(machine, "x") == 1);
		REQUIRE(inner.at(machine, "y") == 2);
	}

	REQUIRE(machine.arena().allocation_counter()
		- machine.arena().deallocation_counter() == allocs_before);

	// A map that the guest builds from scratch, read back by the host
	{
		ScopedCppMap<CppString, CppString> map(machine);
		REQUIRE(int64_t(machine.vmcall<MAX_INSTRUCTIONS>("build_map", map)) == 64);
		REQUIRE(map->size() == 64);

		// Both the iteration and the bucket lookups must work on a map
		// whose nodes and buckets were allocated by the guest
		std::size_t visited = 0;
		map->for_each(machine, [&] (const CppString& key, CppString& value) {
			REQUIRE(key.to_view(machine).substr(0, 3) == "key");
			REQUIRE(value.to_view(machine).substr(0, 5) == "value");
			visited += 1;
		});
		REQUIRE(visited == 64);

		for (int i = 0; i < 64; i++) {
			const auto key = "key" + std::to_string(i);
			CppString* value = map->find(machine, key);
			REQUIRE(value != nullptr);
			REQUIRE(value->to_string(machine) == "value" + std::to_string(i * 3));
		}
		REQUIRE(!map->contains(machine, "key64"));
	}

	REQUIRE(machine.arena().allocation_counter()
		- machine.arena().deallocation_counter() == allocs_before);
}

TEST_CASE("Unordered map system call", "[Native]")
{
	if (is_zig()) // We don't support libc++ std::string yet
		return;

	const auto binary = build_and_load(R"M(
	#include <string>
	#include <unordered_map>
	#include <cassert>
	#define STRINGIFY_HELPER(x) #x
	#define STRINGIFY(x) STRINGIFY_HELPER(x)

	#define GENERATE_SYSCALL_WRAPPER(name, number) \
		__asm__(".global " #name "\n" #name ":\n  li a7, " STRINGIFY(number) "\n  ecall\n  ret\n");
	GENERATE_SYSCALL_WRAPPER(sys_map, 1);
	extern "C" int sys_map(std::unordered_map<std::string, std::string>&);

	void* operator new(size_t size) {
		return malloc(size);
	}
	void operator delete(void* ptr) {
		free(ptr);
	}

	int main() {
		// A map created by the guest, filled in by the host
		std::unordered_map<std::string, std::string> map;
		int ret = sys_map(map);
		assert(ret == 0);
		assert(map.size() == 5);
		assert(map.at("one") == "1");
		assert(map.at("two") == "2");
		assert(map.at("three") == "3");
		assert(map.at("four") == "4");
		assert(map.at("a long key that is not SSO") == "a long value that is not SSO");
		assert(map.find("five") == map.end());

		// Every element must be in the right bucket
		size_t counted = 0;
		for (size_t b = 0; b < map.bucket_count(); b++) {
			for (auto it = map.begin(b); it != map.end(b); ++it) {
				assert(map.bucket(it->first) == b);
				counted += 1;
			}
		}
		assert(counted == map.size());

		// The guest must be able to keep using the map afterwards
		map["five"] = "5";
		map.erase("one");
		assert(map.size() == 5);
		assert(map.at("five") == "5");
		assert(map.find("one") == map.end());

		std::string combined;
		for (const auto& entry : map)
			combined += entry.first + "=" + entry.second + " ";
		assert(combined.size() > 0);

		// Erase everything, so that the destructor has nothing left to do
		map.clear();
		assert(map.empty());
		printf("Unordered map works!\n");
		fflush(stdout);
		return 666;
	})M", "-O2 -static -x c " + cwd + "/include/native_libc.h -x c++ ", true);

	riscv::Machine<RISCV64> machine { binary };
	setup_native_system_calls(machine);
	machine.setup_linux_syscalls();
	machine.setup_linux(
		{"vmcall"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});
	machine.install_syscall_handler(1,
	[] (riscv::Machine<RISCV64>& machine) {
		// The address of the guests own std::unordered_map
		const auto self = machine.sysarg<address_type<RISCV64>>(0);
		auto& map = *machine.memory.memarray<CppMap<CppString, CppString>>(self, 1);
		REQUIRE(map.empty());

		map.assign(machine, self, std::unordered_map<std::string, std::string> {
			{"one", "1"}, {"two", "2"}, {"three", "3"}, {"four", "4"},
			{"a long key that is not SSO", "a long value that is not SSO"}
		});
		REQUIRE(map.size() == 5);
		REQUIRE(map.at(machine, "two").to_string(machine) == "2");
		machine.set_result(0);
	});

	bool output_ok = false;
	machine.set_userdata(&output_ok);
	machine.set_printer([] (const auto& m, const char* data, size_t size) {
		std::string text{data, data + size};
		bool* ok = m.template get_userdata<bool> ();
		*ok = *ok || text.find("Unordered map works!") != std::string::npos;
	});

	machine.simulate(MAX_INSTRUCTIONS);
	REQUIRE(machine.return_value<int>() == 666);
	REQUIRE(output_ok);
}

TEST_CASE("Vector of strings system call", "[Native]")
{
	if (is_zig()) // We don't support libc++ std::string yet
		return;

	const auto binary = build_and_load(R"M(
	#include <string>
	#include <vector>
	#include <cassert>
	#define STRINGIFY_HELPER(x) #x
	#define STRINGIFY(x) STRINGIFY_HELPER(x)

	#define GENERATE_SYSCALL_WRAPPER(name, number) \
		__asm__(".global " #name "\n" #name ":\n  li a7, " STRINGIFY(number) "\n  ecall\n  ret\n");
	GENERATE_SYSCALL_WRAPPER(sys_vector, 1);
	extern "C" int sys_vector(const std::vector<std::string>&);

	int main() {
		std::vector<std::string> vec;
		int ret = sys_vector(vec);
		assert(ret == 0);
		assert(vec.size() == 5);
		assert(vec[0] == "Syscall");
		assert(vec[1] == "vector");
		assert(vec[2] == "of");
		assert(vec[3] == "strings");
		assert(vec[4] == "works!");

		std::string combined;
		for (const auto& s : vec) {
			combined += s + " ";
		}
		printf("Combined string: %s\n", combined.c_str());
		fflush(stdout);
		return 666;
	})M", "-O2 -static -x c " + cwd + "/include/native_libc.h -x c++ ", true);

	riscv::Machine<RISCV64> machine { binary };
	setup_native_system_calls(machine);
	machine.setup_linux_syscalls();
	machine.setup_linux(
		{"vmcall"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});
	machine.install_syscall_handler(1,
	[] (riscv::Machine<RISCV64>& machine) {
		// Syscall to test std::vector<std::string>
		auto [vec] = machine.sysargs<CppVector<CppString>*>();
		vec->assign(machine,
			std::vector<std::string>{ "Syscall", "vector", "of", "strings", "works!" });
		machine.set_result(0);
	});

	machine.set_printer([] (const auto&, const char* data, size_t size) {
		std::string text{data, data + size};
		printf("%s", text.c_str());
		fflush(stdout);
		REQUIRE(text == "Combined string: Syscall vector of strings works! \n");
	});

	machine.simulate(MAX_INSTRUCTIONS);
	REQUIRE(machine.return_value<int>() == 666);
}
