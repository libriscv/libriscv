#include <heap.hpp>
#include <include/libc.hpp>
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
void* realloc(void* ptr, size_t newsize)
{
	return sys_realloc(ptr, newsize);
}
extern "C"
void free(void* ptr)
{
	return sys_free(ptr);
}
