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
void* realloc(void* old, size_t newsize)
{
	void* newalloc = malloc(newsize);
	// NOTE: this will not always work
	return memcpy(newalloc, old, newsize);
}
extern "C"
void free(void* ptr)
{
	return sys_free(ptr);
}
