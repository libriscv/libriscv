/**
 * Accelerated heap using syscalls
 *
**/
#pragma once
#include <cstddef>
#include "include/syscall.hpp"

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

struct MemInfo {
	size_t bytes_free;
	size_t bytes_used;
	size_t chunks_used;
};
inline int sys_meminfo(void* ptr, size_t len)
{
	return psyscall(SYSCALL_MEMINFO, ptr, len);
}
