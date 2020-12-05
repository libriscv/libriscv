#include <heap.hpp>
#include <include/libc.hpp>
#include <cstdlib>

#if 1

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
	sys_free(ptr);
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
	sys_free(ptr);
}

#else

static const uintptr_t sbrk_start = 0x40000000;
static const uintptr_t sbrk_max   = sbrk_start + 0x1000000;

extern "C"
long _sbrk(uintptr_t new_end)
{
	static uintptr_t sbrk_end = sbrk_start;
    if (new_end == 0) return sbrk_end;
    sbrk_end = new_end;
    sbrk_end = std::max(sbrk_end, sbrk_start);
    sbrk_end = std::min(sbrk_end, sbrk_max);
	return sbrk_end;
}

#endif
