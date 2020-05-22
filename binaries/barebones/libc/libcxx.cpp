#include <cstddef>
#include "heap.hpp"
extern "C"
__attribute__((noreturn)) void abort_message(const char* fmt, ...);
extern "C" void __cxa_guard_acquire(void*) {}
extern "C" void __cxa_guard_release(void*) {}

void* operator new(size_t size)
{
	return sys_malloc(size);
}
void* operator new[](size_t size)
{
	return sys_malloc(size);
}

void operator delete(void* ptr)
{
	sys_free(ptr);
}
void operator delete[](void* ptr)
{
	sys_free(ptr);
}
// C++14 sized deallocation
void operator delete(void* ptr, std::size_t)
{
	sys_free(ptr);
}
void operator delete [](void* ptr, std::size_t)
{
	sys_free(ptr);
}

// exception stubs for various C++ containers
namespace std {
	void __throw_length_error(char const*) {
		abort_message("C++ length error exception");
	}
	void __throw_logic_error(char const*) {
		abort_message("C++ length error exception");
	}
	void __throw_out_of_range_fmt(char const*, ...) {
		abort_message("C++ out-of-range exception");
	}
	void __throw_bad_function_call() {
		abort_message("Bad std::function call!");
	}
}

extern "C"
int __cxa_atexit(void (*func) (void*), void* /*arg*/, void* /*dso_handle*/)
{
	(void) func;
	return 0;
}
