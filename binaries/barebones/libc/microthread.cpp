#include "microthread.hpp"

extern "C" void microthread_set_tp(void*);

namespace microthread
{
	Thread main_thread {nullptr};

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

	__attribute__((constructor, used))
	static void init_threads()
	{
		microthread_set_tp(&main_thread);
	}
}

asm(".section .text\n"
".global microthread_set_tp\n"
".type microthread_set_tp, @function\n"
"microthread_set_tp:\n"
"  mv tp, a0\n"
"  ret\n");

#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)

// This function never returns (so no ret)
asm(".global threadcall_destructor\n"
".type threadcall_destructor, @function\n"
"threadcall_destructor:\n"
"	li a7, " STRINGIFY(THREAD_SYSCALLS_BASE+9) "\n"
"	ecall\n");
