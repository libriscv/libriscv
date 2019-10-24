#include <cstddef>
#include <memory>
#include <new>
extern "C"
__attribute__((noreturn)) void abort_message(const char* fmt, ...);


void* operator new(size_t size)
{
  void* res = std::malloc(size);
  return res;
}
void* operator new[](size_t size)
{
  void* res = std::malloc(size);
  return res;
}

void operator delete(void* ptr)
{
  std::free(ptr);
}
void operator delete[](void* ptr)
{
  std::free(ptr);
}
// C++14 sized deallocation
void operator delete(void* ptr, std::size_t)
{
  std::free(ptr);
}
void operator delete [](void* ptr, std::size_t)
{
  std::free(ptr);
}

// exception stubs for various C++ containers
namespace std {
  void __throw_length_error(char const*) {
	  abort_message("C++ length error exception");
  }
}
