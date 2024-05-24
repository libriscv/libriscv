#include "api.hpp"
#include "dyncall.hpp"
#include <cstdio>
#include <memory>
#include <vector>

// A dynamic call for testing integer arguments and return values
DEFINE_DYNCALL(1, dyncall1, int(int));
// A dynamic call for testing string arguments
DEFINE_DYNCALL(2, dyncall2, void(const char*, size_t, const char*));
// A dynamic call for benchmarking the overhead of dynamic calls
DEFINE_DYNCALL(3, dyncall_empty, void());
// A dynamic call that passes a view to complex data
struct MyData {
	char buffer[32];
};
DEFINE_DYNCALL(4, dyncall_data, void(const MyData*, size_t, const MyData&));

// A function that retrieves the contents of a location,
// or returns a nullptr if the location is not found.
struct LocationGet {
	uint8_t* data;
	size_t size = 0;
};
DEFINE_DYNCALL(10, location_get, LocationGet(int, int, int));
// A function that commits the contents of a location
// It cannot return an error, instead it will throw an exception.
DEFINE_DYNCALL(11, location_commit, void(int, int, int, const void*, size_t));

DEFINE_DYNCALL(12, remote_lambda, void(void(*)(void*), const void *, size_t));

#include "../../../lib/libriscv/util/function.hpp"
static void rpc(riscv::Function<void()> func)
{
	remote_lambda(
		[](void* data) {
			auto func = reinterpret_cast<riscv::Function<void()>*>(data);
			(*func)();
		},
		&func, sizeof(func));
}

#include <span>
struct LocationData {
	LocationData(int x, int y, int z)
		: x(x), y(y), z(z)
	{
		auto res = location_get(x, y, z);
		if (res.data) {
			m_data.reset(res.data);
			m_size = res.size;
		}
	}
	void commit() {
		location_commit(x, y, z, m_data.get(), m_size);
	}

	bool empty() const noexcept {
		return m_data == nullptr || m_size == 0;
	}
	std::span<uint8_t> data() {
		return { m_data.get(), m_size };
	}
	void assign(const uint8_t* data, size_t size) {
		m_data = std::make_unique<uint8_t[]>(size);
		std::copy(data, data + size, m_data.get());
		m_size = size;
	}

	const int x, y, z;
private:
	std::unique_ptr<uint8_t[]> m_data = nullptr;
	std::size_t m_size = 0;
};

DEFINE_DYNCALL(13, my_callback, void(const char*, void(*)(int, void*), const void*, size_t));

static void entity_on_event(const char* name, riscv::Function<void(int)> callback)
{
	my_callback(name,
	[] (int id, void* data) {
		auto callback = reinterpret_cast<riscv::Function<void(int)>*>(data);
		(*callback)(id);
	},
	&callback, sizeof(callback));
}

// Every instantiated program runs through main()
int main(int argc, char** argv)
{
	printf("Hello, World from a RISC-V virtual machine!\n");

	// Register a callback for an entity
	int x = 42;
	entity_on_event("entity1",
	[x] (int id) {
		//printf("Callback from entity %s\n", Entity{id}.getName().c_str());
		printf("x = %d\n", x);
	});

	// Call a function that was registered as a dynamic call
	auto result = dyncall1(0x12345678);
	printf("dyncall1(1) = %d\n", result);

	// Call a function that passes a string (with length)
	dyncall2("Hello, Vieworld!", 16, "A zero-terminated string!");

	// Printf uses an internal buffer, so we need to flush it
	fflush(stdout);

	// Test LocationGet and LocationCommit
	LocationData loc(1, 2, 3);
	if (!loc.empty()) {
		printf("Location (1, 2, 3) contains %zu bytes\n", loc.data().size());
		location_commit(1, 2, 3, loc.data().data(), loc.data().size());
	} else {
		printf("LocationGet(1, 2, 3) was empty!\n");
	}

	std::vector<uint8_t> data = { 0x01, 0x02, 0x03, 0x04 };
	loc.assign(data.data(), data.size());
	loc.commit();

	LocationData loc2(1, 2, 3);
	if (!loc2.empty()) {
		printf("Location (1, 2, 3) contains %zu bytes\n", loc2.data().size());
		location_commit(1, 2, 3, loc2.data().data(), loc2.data().size());
	} else {
		printf("LocationGet(1, 2, 3) was empty!\n");
	}

	// Test remote lambda
	x = 42;
	rpc([x] {
		printf("Hello from a remote virtual machine!\n");
		printf("x = %d\n", x);
		fflush(stdout);
	});

	// Let's avoid calling global destructors, as they have a tendency
	// to make global variables unusable before we're done with them.
	// Remember, we're going to be making function calls after this.
	fast_exit(0);
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
#ifdef __cpp_lib_smart_ptr_for_overwrite
	auto x = std::make_unique_for_overwrite<char[]>(1024);
#else
	auto x = std::unique_ptr<char[]>(new char[1024]);
#endif
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

PUBLIC(void test5())
{
	std::vector<MyData> vec;
	vec.push_back(MyData{ "Hello, World!" });
	MyData data = { "Second data!" };

	dyncall_data(vec.data(), vec.size(), data);
}

// This function is used to benchmark the overhead of dynamic calls.
PUBLIC(void bench_dyncall_overhead())
{
	dyncall_empty();
}
