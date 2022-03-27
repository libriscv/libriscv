/**
 * Accelerated heap using syscalls
 *
**/
#pragma once
#include <cstddef>
#include <include/syscall.hpp>

struct MemInfo {
	size_t bytes_free;
	size_t bytes_used;
	size_t chunks_used;
};

extern "C" void* sys_malloc(size_t);
extern "C" void* sys_calloc(size_t, size_t);
extern "C" void* sys_realloc(void*, size_t);
extern "C" long  sys_free(void*);

inline int sys_meminfo(void* ptr, size_t len)
{
	return psyscall(SYSCALL_MEMINFO, ptr, len);
}
