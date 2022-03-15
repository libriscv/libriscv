#include "microthread.hpp"

namespace microthread
{
	void trampoline(Thread* thread)
	{
		thread->startfunc();
	}
	void oneshot_exit()
	{
		free(self()); // after this point stack unusable
		syscall(THREAD_SYSCALLS_BASE+1, 0);
		__builtin_unreachable();
	}

	static Thread main_thread {nullptr};
	__attribute__((constructor, naked, used))
	void init_threads()
	{
		register long tp asm("tp");
		asm volatile("mv %0, %1" : "=r"(tp) : "r"(&main_thread));
		asm volatile("ret");
	}
}

#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)

// This function never returns (so no ret)
asm(".global threadcall_destructor\n"
".type threadcall_destructor, @function\n"
"threadcall_destructor:\n"
"	li a7, " STRINGIFY(THREAD_SYSCALLS_BASE+9) "\n"
"	ecall\n");
