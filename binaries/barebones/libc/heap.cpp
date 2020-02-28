#include "heap.hpp"
#include <cstdlib>

extern "C"
void* malloc(size_t size)
{
	return sys_malloc(size);
}
extern "C"
void* calloc(size_t count, size_t size)
{
	return sys_calloc(count, size);
}
extern "C"
void free(void* ptr)
{
	return sys_free(ptr);
}
