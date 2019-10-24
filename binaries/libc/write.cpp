#include <include/printf.h>
#include <string.h>
#include <include/syscall.hpp>

extern "C"
long write(int fd, const void* data, size_t len)
{
	return syscall(SYSCALL_WRITE, fd, (long) data, len);
}

extern "C"
int puts(const char* string)
{
	syscall(SYSCALL_WRITE, 0, (long) "PUTS\n", 5);
	const long len = strlen(string);
	return write(0, string, len);
}

// buffered serial output
static char buffer[256];
static unsigned cnt = 0;

extern "C"
int fflush(FILE* fileno)
{
	(void) fileno;
	long ret = write(0, buffer, cnt);
	cnt = 0;
	return ret;
}

extern "C"
void __print_putchr(const void* file, char c)
{
	syscall(SYSCALL_WRITE, 0, (long) "PUTCHAR\n", 8);
	(void) file;
	buffer[cnt++] = c;
	if (c == '\n' || cnt == sizeof(buffer)) {
		fflush(0);
	}
}
