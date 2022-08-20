#include <cstdint>
#include <cstdlib>
#include <heap.hpp>
#include <include/libc.hpp>
#ifdef USE_NEWLIB
#include <malloc.h>
#endif
#define NATIVE_MEM_FUNCATTR __attribute__((noinline, used))
#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)
//#define VERBOSE_HEAP

#if 1

#if defined(WRAP_NATIVE_SYSCALLS)
#define malloc  __wrap_malloc
#define calloc  __wrap_calloc
#define realloc __wrap_realloc
#define free    __wrap_free
#define _sbrk    __wrap__sbrk
#define _malloc_r    __wrap__malloc_r
#define _calloc_r    __wrap__calloc_r
#define _realloc_r   __wrap__realloc_r
#define _free_r      __wrap__free_r
#define _memalign_r  __wrap__memalign_r
#define memalign       __wrap_memalign
#define aligned_alloc  __wrap_aligned_alloc
#define posix_memalign __wrap_posix_memalign
#endif

#define GENERATE_SYSCALL_WRAPPER(name, number) \
	asm(".global " #name "\n" #name ":\n  li a7, " STRINGIFY(number) "\n  ecall\n  ret\n");

GENERATE_SYSCALL_WRAPPER(sys_malloc,  SYSCALL_MALLOC);
GENERATE_SYSCALL_WRAPPER(sys_calloc,  SYSCALL_CALLOC);
GENERATE_SYSCALL_WRAPPER(sys_realloc, SYSCALL_REALLOC);
GENERATE_SYSCALL_WRAPPER(sys_free,    SYSCALL_FREE);

extern "C" NATIVE_MEM_FUNCATTR
void* malloc(size_t size)
{
	void* result = sys_malloc(size);
	#ifdef VERBOSE_HEAP
		fmt_print("malloc(", size, ") = ", result);
		if (result == nullptr)
			fmt_print("** WARNING: malloc(", size, ") FAILED");
	#endif
	return result;
}
extern "C" NATIVE_MEM_FUNCATTR
void* calloc(size_t count, size_t size)
{
	void* result = sys_calloc(count, size);
	#ifdef VERBOSE_HEAP
		fmt_print("calloc(", count * size, ") = ", result);
		if (result == nullptr)
			fmt_print("** WARNING: calloc(", count * size, ") FAILED");
	#endif
	return result;
}
extern "C" NATIVE_MEM_FUNCATTR
void* realloc(void* ptr, size_t newsize)
{
	void* result = sys_realloc(ptr, newsize);
	#ifdef VERBOSE_HEAP
		fmt_print("realloc(", ptr, ", ", newsize, ") = ", result);
		if (result == nullptr)
			fmt_print("** WARNING: realloc(", ptr, ", ", newsize, ") FAILED");
	#endif
	return result;
}
extern "C" NATIVE_MEM_FUNCATTR
void free(void* ptr)
{
	int result = sys_free(ptr);
	#ifdef VERBOSE_HEAP
		fmt_print("free(", ptr, ") = ", result);
		if (result < 0)
			fmt_print("** WARNING: free(", ptr, ") FAILED");
	#endif
	(void) result;
}
extern "C" NATIVE_MEM_FUNCATTR
void* reallocf(void *ptr, size_t newsize)
{
	void* newptr = realloc(ptr, newsize);
	if (newptr == nullptr) free(ptr);
	return newptr;
}
extern "C" NATIVE_MEM_FUNCATTR
void* memalign(size_t align, size_t bytes)
{
	// XXX: TODO: Make an accelerated memalign system call
	void* freelist[1024]; // Enough for 4K alignment
	size_t freecounter = 0;
	void* ptr = nullptr;

	while (true) {
		ptr = sys_malloc(bytes);
		if (ptr == nullptr) break;
		bool aligned = ((uintptr_t)ptr & (align-1)) == 0;
		if (aligned) break;
		sys_free(ptr);
		// Allocate 8 bytes to advance the next pointer
		freelist[freecounter++] = sys_malloc(8);
	}

	for (size_t i = 0; i < freecounter; i++) sys_free(freelist[i]);
	#ifdef VERBOSE_HEAP
		fmt_print("memalign(", align, ", bytes = ", bytes, ") = ", ptr);
	#endif
	return ptr;
}
extern "C" NATIVE_MEM_FUNCATTR
int posix_memalign(void **memptr, size_t alignment, size_t size)
{
	void* ptr = memalign(alignment, size);
	*memptr = ptr;
	return 0;
}
extern "C" NATIVE_MEM_FUNCATTR
void* aligned_alloc(size_t alignment, size_t size)
{
	return memalign(alignment, size);
}

// Re-entrant newlib internal versions
extern "C" NATIVE_MEM_FUNCATTR
void* _malloc_r(_reent*, size_t bytes) {
	return malloc(bytes);
}
extern "C" NATIVE_MEM_FUNCATTR
void* _calloc_r(_reent*, size_t count, size_t size) {
	return calloc(count, size);
}
extern "C" NATIVE_MEM_FUNCATTR
void* _realloc_r(_reent*, void* ptr, size_t bytes) {
	return realloc(ptr, bytes);
}
extern "C" NATIVE_MEM_FUNCATTR
void _free_r(_reent*, void* ptr) {
	free(ptr);
}
extern "C" NATIVE_MEM_FUNCATTR
void* _memalign_r(_reent*, size_t align, size_t bytes) {
	return memalign(align, bytes);
}

// These newlib internal functions are disabled now
extern "C"
uintptr_t _sbrk(uintptr_t /*new_end*/)
{
	asm("unimp");
	__builtin_unreachable();
}

#else

static const uintptr_t sbrk_start = 0xF0000000;
static const uintptr_t sbrk_max   = 0xFF000000;

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
