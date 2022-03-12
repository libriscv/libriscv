#include <heap.hpp>
#include <include/libc.hpp>
#include <cstdlib>

#ifdef USE_NEWLIB
#define malloc  __wrap_malloc
#define calloc  __wrap_calloc
#define realloc __wrap_realloc
#define free    __wrap_free
#endif

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

#else

static const uintptr_t sbrk_start = 0x40000000;
static const uintptr_t sbrk_max   = sbrk_start + 0x2000000;

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
