#include <heap.hpp>
#include <include/libc.hpp>
#include <cstdlib>
#define NATIVE_MEM_FUNCATTR /* */

#if defined(USE_NEWLIB) && defined(WRAP_NATIVE_SYSCALLS)
#define malloc  __wrap_malloc
#define calloc  __wrap_calloc
#define realloc __wrap_realloc
#define free    __wrap_free
#endif

#if 1

extern "C" NATIVE_MEM_FUNCATTR
void* malloc(size_t size)
{
	return sys_malloc(size);
}
extern "C" NATIVE_MEM_FUNCATTR
void* calloc(size_t count, size_t size)
{
	register size_t  a0 asm("a0") = count;
	register size_t  a1 asm("a1") = size;
	register long syscall_id asm("a7") = SYSCALL_CALLOC;
	register void*   a0_out asm("a0");

	asm volatile ("ecall"
		:	"=r"(a0_out)
		:	"r"(a0), "r"(a1), "r"(syscall_id));
	return a0_out;
}
extern "C" NATIVE_MEM_FUNCATTR
void* realloc(void* ptr, size_t newsize)
{
	register void*   a0 asm("a0") = ptr;
	register size_t  a1 asm("a1") = newsize;
	register long syscall_id asm("a7") = SYSCALL_REALLOC;

	asm volatile ("ecall"
		:	"+r"(a0)
		:	"r"(a1), "r"(syscall_id));
	return a0;
}
extern "C" NATIVE_MEM_FUNCATTR
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
