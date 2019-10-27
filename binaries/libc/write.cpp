#include <include/printf.h>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <include/syscall.hpp>

extern "C"
long write(int fd, const void* data, size_t len)
{
	return syscall(SYSCALL_WRITE, fd, (long) data, len, 0, 0, 0);
}

extern "C"
long sendint(uint32_t value)
{
	return syscall(SYSCALL_SINT, value, 0, 0, 0, 0, 0);
}

extern "C"
int puts(const char* string)
{
	const long len = strlen(string);
	return write(0, string, len);
}

// buffered serial output
static char buffer[256];
static unsigned cnt = 0;

extern "C"
int fflush(FILE*)
{
	long ret = write(0, buffer, cnt);
	cnt = 0;
	return ret;
}

extern "C"
void __print_putchr(const void*, char c)
{
	buffer[cnt++] = c;
	if (c == '\n' || cnt == sizeof(buffer)) {
		fflush(0);
	}
}
