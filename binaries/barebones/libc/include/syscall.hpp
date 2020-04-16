#pragma once

#define SYSCALL_MALLOC   1
#define SYSCALL_CALLOC   2
#define SYSCALL_REALLOC  3
#define SYSCALL_FREE     4

#define SYSCALL_WRITE  64
#define SYSCALL_EXIT   93

inline long syscall(long n)
{
	register long a0 asm("a0");
	register long syscall_id asm("a7") = n;

	asm volatile ("scall" : "=r"(a0) : "r"(syscall_id));

	return a0;
}

inline long syscall(long n, long arg0)
{
	register long a0 asm("a0") = arg0;
	register long syscall_id asm("a7") = n;

	asm volatile ("scall" : "+r"(a0) : "r"(syscall_id));

	return a0;
}

inline long syscall(long n, long arg0, long arg1)
{
	register long a0 asm("a0") = arg0;
	register long a1 asm("a1") = arg1;
	register long syscall_id asm("a7") = n;

	asm volatile ("scall" : "+r"(a0) : "r"(a1), "r"(syscall_id));

	return a0;
}

inline long syscall(long n, long arg0, long arg1, long arg2)
{
	register long a0 asm("a0") = arg0;
	register long a1 asm("a1") = arg1;
	register long a2 asm("a2") = arg2;
	register long syscall_id asm("a7") = n;

	asm volatile ("scall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(syscall_id));

	return a0;
}

inline long syscall(long n, long arg0, long arg1, long arg2, long arg3)
{
	register long a0 asm("a0") = arg0;
	register long a1 asm("a1") = arg1;
	register long a2 asm("a2") = arg2;
	register long a3 asm("a3") = arg3;
	register long syscall_id asm("a7") = n;

	asm volatile ("scall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(syscall_id));

	return a0;
}

inline long
syscall(long n, long arg0, long arg1, long arg2, long arg3, long arg4)
{
	register long a0 asm("a0") = arg0;
	register long a1 asm("a1") = arg1;
	register long a2 asm("a2") = arg2;
	register long a3 asm("a3") = arg3;
	register long a4 asm("a4") = arg4;
	register long syscall_id asm("a7") = n;

	asm volatile ("scall"
		: "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(syscall_id));

	return a0;
}

inline long
syscall(long n, long arg0, long arg1, long arg2, long arg3, long arg4, long arg5)
{
	register long a0 asm("a0") = arg0;
	register long a1 asm("a1") = arg1;
	register long a2 asm("a2") = arg2;
	register long a3 asm("a3") = arg3;
	register long a4 asm("a4") = arg4;
	register long a5 asm("a5") = arg5;
	// NOTE: only 16 regs in RV32E instruction set
	register long syscall_id asm("a7") = n;

	asm volatile ("scall"
		: "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(syscall_id));

	return a0;
}

inline long
syscall(long n, long arg0, long arg1, long arg2,
		long arg3, long arg4, long arg5, long arg6)
{
	register long a0 asm("a0") = arg0;
	register long a1 asm("a1") = arg1;
	register long a2 asm("a2") = arg2;
	register long a3 asm("a3") = arg3;
	register long a4 asm("a4") = arg4;
	register long a5 asm("a5") = arg5;
	register long a6 asm("a6") = arg6;
	register long syscall_id asm("a7") = n;

	asm volatile ("scall"
		: "+r"(a0) : "r"(a1), "r"(a2), "r"(a3),
			"r"(a4), "r"(a5), "r"(a6), "r"(syscall_id));

	return a0;
}
