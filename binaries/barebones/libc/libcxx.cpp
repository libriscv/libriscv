#include <cstddef>
#include <new>
#include "heap.hpp"
extern "C"
__attribute__((noreturn)) void abort_message(const char* fmt, ...);

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
}
