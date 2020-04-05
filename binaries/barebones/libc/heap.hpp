/**
 * Accelerated heap using syscalls
 *
**/
#include <cstddef>
#include "include/syscall.hpp"

#define SYSCALL_MALLOC   1
#define SYSCALL_CALLOC   2
#define SYSCALL_REALLOC  3
#define SYSCALL_FREE     4

inline void* sys_malloc(size_t len)
{
	return (void*) syscall(SYSCALL_MALLOC, len);
}
inline void* sys_calloc(size_t count, size_t size)
{
	return (void*) syscall(SYSCALL_CALLOC, count, size);
}
inline void* sys_realloc(void* ptr, size_t len)
{
	return (void*) syscall(SYSCALL_REALLOC, (long) ptr, len);
}
inline void sys_free(void* ptr)
{
	syscall(SYSCALL_FREE, (long) ptr);
}
