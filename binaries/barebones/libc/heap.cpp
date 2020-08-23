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

/* Newlib internal reentrancy-safe heap functions.
   Our system calls are safe because they are atomic. */
extern "C"
void* _malloc_r(struct _reent*, size_t size)
{
	return sys_malloc(size);
}
extern "C"
void* _calloc_r(struct _reent*, size_t count, size_t size)
{
	return sys_calloc(count, size);
}
extern "C"
void* _realloc_r(struct _reent*, void* ptr, size_t newsize)
{
	return sys_realloc(ptr, newsize);
}
extern "C"
void _free_r(struct _reent*, void* ptr)
{
	return sys_free(ptr);
}
