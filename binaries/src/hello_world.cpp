#include <include/syscall.h>
#include <include/libc.hpp>

#define STDIN  0
#define STDOUT 1
#define STDERR 2

static inline
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

int main(int, char**)
{
	const char hello_world[] = "Hello RISC-V World!\n";
	int bytes = write(STDOUT, hello_world, sizeof(hello_world)-1);
	syscall(666, bytes, 0);

	const char write_str[] = "write: ";
	const int write_str_len = sizeof(write_str)-1;
	char buffer[512];
	memcpy(buffer, write_str, write_str_len);

	char* bend = int32_to_str(&buffer[write_str_len], bytes);
	bend[0] = '\n';
	write(STDOUT, buffer, bend - buffer + 1);

	asm("nop");
	return 666;
}
