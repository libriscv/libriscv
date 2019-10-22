extern "C" {
	static long
	syscall(long n, long a0, long a1 = 0, long a2 = 0, long a3 = 0, long a4 = 0, long a5 = 0);

	void _start();
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

#ifdef __riscv_32e
	register long syscall_id asm("t0") = n;
#else
	register long syscall_id asm("a7") = n;
#endif

	asm volatile ("scall"
		: "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(syscall_id));

	return a0;
}

#define SYSCALL_WRITE  64
#define SYSCALL_EXIT   93

#define STDIN  0
#define STDOUT 1
#define STDERR 2

void _start()
{
	static const char hello_world[] = "Hello RISC-V World!\n";

	syscall(SYSCALL_WRITE, STDOUT, (long) hello_world, sizeof(hello_world));

	syscall(SYSCALL_EXIT, 666);
}
