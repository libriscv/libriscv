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

DEFINE_DYNCALL(1, dyncall1); // int(int)
DEFINE_DYNCALL(2, dyncall2); // void(const char*, size_t, const char*)
DEFINE_DYNCALL(3, dyncall_empty); // void()
struct MyData {
	char buffer[32];
};
DEFINE_DYNCALL(4, dyncall_data); // void(const MyData*, size_t, const MyData*)
