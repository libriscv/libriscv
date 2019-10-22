#include "../src/syscall.h"

#define STDIN  0
#define STDOUT 1
#define STDERR 2

extern "C"
void _start()
{
	static const char hello_world[] = "Hello RISC-V World!\n";

	syscall(SYSCALL_WRITE, STDOUT, (long) hello_world, sizeof(hello_world));

	syscall(SYSCALL_EXIT, 666);
}
