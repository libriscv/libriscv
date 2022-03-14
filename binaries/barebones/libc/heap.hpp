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

inline void* sys_malloc(size_t len)
{
	register size_t  a0 asm("a0") = len;
	register long syscall_id asm("a7") = SYSCALL_MALLOC;
	register void*   a0_out asm("a0");

	asm volatile ("ecall"
		:	"=r"(a0_out)
		:	"r"(a0), "r"(syscall_id));
	return a0_out;
}
inline int sys_free(void* ptr)
{
	register void*   a0 asm("a0") = ptr;
	register long syscall_id asm("a7") = SYSCALL_FREE;
	register long    a0_out asm("a0");

	asm volatile ("ecall"
		:	"=r"(a0_out)
		:	"r"(a0), "r"(syscall_id));
	return a0_out;
}

inline int sys_meminfo(void* ptr, size_t len)
{
	return psyscall(SYSCALL_MEMINFO, ptr, len);
}
