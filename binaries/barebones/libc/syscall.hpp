#pragma once

inline long
syscall1(long n, long arg0)
{
	register long a0 asm("a0") = arg0;
	register long syscall_id asm("a7") = n;

	asm volatile ("scall" : "+r"(a0) : "r"(syscall_id));

	return a0;
}

inline long
syscall2(long n, long arg0, long arg1)
{
	register long a0 asm("a0") = arg0;
	register long a1 asm("a1") = arg1;
	register long syscall_id asm("a7") = n;

	asm volatile ("scall" : "+r"(a0) : "r"(a1), "r"(syscall_id));

	return a0;
}
