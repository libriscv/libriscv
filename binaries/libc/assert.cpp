#include <include/syscall.hpp>
#include <include/printf.h>
#include <cstdarg>

extern "C"
__attribute__((noreturn))
void panic(const char* reason)
{
	printf("\n\n!!! PANIC !!!\n%s\n", reason);

	// the end
	syscall(SYSCALL_EXIT, -1);
	__builtin_unreachable();
}

extern "C"
void abort()
{
	panic("Abort called");
}

extern "C"
void abort_message(const char* fmt, ...)
{
	char buffer[512];
	va_list arg;
	va_start (arg, fmt);
	int bytes = vsnprintf(buffer, sizeof(buffer), fmt, arg);
	(void) bytes;
	va_end (arg);
	panic(buffer);
}

extern "C"
void __assert_func(
	const char *file,
	int line,
	const char *func,
	const char *failedexpr)
{
	printf(
		"assertion \"%s\" failed: file \"%s\", line %d%s%s\n",
		failedexpr, file, line,
		func ? ", function: " : "", func ? func : "");
	abort();
}
