#include "../src/syscall.h"

#define STDIN  0
#define STDOUT 1
#define STDERR 2

//static inline
char* int32_to_str(char* b, int val)
{
	// negation
	if (val < 0) { *b++ = '-'; val = -val; }
	// move to end of repr.
	int tmp = val;
	do { ++b; tmp /= 10;  } while (tmp);
	char* end = b;
	*end = '\0';
	// move back and insert as we go
	do {
		*--b = '0' + (val % 10);
		val /= 10;
	} while (val);
	return end;
}

extern "C"
inline long write(int fd, const void* buffer, long len)
{
	return syscall(SYSCALL_WRITE, fd, (long) buffer, len);
}

extern "C"  __attribute__((noreturn))
void _exit(int exitval) {
	syscall(SYSCALL_EXIT, exitval, 0);
	__builtin_unreachable();
}

extern "C"
void _start()
{
	const char hello_world[] = "Hello RISC-V World!\n";
	int bytes = write(STDOUT, hello_world, sizeof(hello_world)-1);
	syscall(666, bytes, 0);

	char buffer[512];
	buffer[0] = 'w';
	buffer[1] = 'r';
	buffer[2] = 'i';
	buffer[3] = 't';
	buffer[4] = 'e';
	buffer[5] = ':';
	buffer[6] = ' ';
	char* bend = int32_to_str(&buffer[7], bytes);
	bend[0] = '\n';
	write(STDOUT, buffer, bend - buffer + 1);

	asm("nop");

	_exit(666);
}
