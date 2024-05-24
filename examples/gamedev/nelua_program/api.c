#include <stddef.h>

#define ECALL_WRITE  64
#define ECALL_EXIT   93

int my_write(int fd, const char *buffer, size_t size)
{
	register int         a0 __asm__("a0")  = fd;
	register const char* a1 __asm__("a1")  = buffer;
	register size_t a2 __asm__("a2")	   = size;
	register long syscall_id __asm__("a7") = ECALL_WRITE;

	__asm__ volatile("ecall"
				 : "+r"(a0)
				 : "m"(*(const char(*)[size])a1), "r"(a2),
				   "r"(syscall_id));
	return a0;
}

void fast_exit(int status)
{
	register int         a0 __asm__("a0")  = status;
	register long syscall_id __asm__("a7") = ECALL_EXIT;

	__asm__ volatile("ecall" : : "r"(a0), "r"(syscall_id));
	__builtin_unreachable();
}

void measure_overhead() {}


#define DEFINE_DYNCALL(number, name) \
	asm(".pushsection .text\n" \
		".global " #name "\n" \
		".func " #name "\n" \
		"" #name ":\n" \
		"	li t0, " #number "\n" \
		"   li a7, 510\n" \
		"   ecall\n" \
		"   ret\n"   \
		".endfunc\n" \
		".popsection .text\n"); \
	extern void name() __attribute__((used));

DEFINE_DYNCALL(1, dyncall1);
DEFINE_DYNCALL(2, dyncall2);
DEFINE_DYNCALL(3, dyncall3);
DEFINE_DYNCALL(4, dyncall4);
DEFINE_DYNCALL(5, dyncall5);
DEFINE_DYNCALL(6, dyncall6);
DEFINE_DYNCALL(7, dyncall7);
DEFINE_DYNCALL(8, dyncall8);
DEFINE_DYNCALL(9, dyncall9);
DEFINE_DYNCALL(10, dyncall10);
DEFINE_DYNCALL(11, dyncall11);
DEFINE_DYNCALL(12, dyncall12);
DEFINE_DYNCALL(13, dyncall13);
DEFINE_DYNCALL(14, dyncall14);
DEFINE_DYNCALL(15, dyncall15);
DEFINE_DYNCALL(16, dyncall16);
DEFINE_DYNCALL(17, dyncall17);
DEFINE_DYNCALL(18, dyncall18);
DEFINE_DYNCALL(19, dyncall19);
DEFINE_DYNCALL(20, dyncall20);
DEFINE_DYNCALL(21, dyncall21);
DEFINE_DYNCALL(22, dyncall22);
DEFINE_DYNCALL(23, dyncall23);
DEFINE_DYNCALL(24, dyncall24);
DEFINE_DYNCALL(25, dyncall25);
DEFINE_DYNCALL(26, dyncall26);
DEFINE_DYNCALL(27, dyncall27);
DEFINE_DYNCALL(28, dyncall28);
DEFINE_DYNCALL(29, dyncall29);
DEFINE_DYNCALL(30, dyncall30);
DEFINE_DYNCALL(31, dyncall31);
DEFINE_DYNCALL(32, dyncall32);

