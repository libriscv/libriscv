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
	void direct_starter(Thread* thread)
	{
		thread->tinyfunc();
		oneshot_exit();
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

extern "C"
__attribute__((noinline))
long threadcall_executor(...)
{
	return syscall(THREAD_SYSCALLS_BASE+8);
}

extern "C"
void threadcall_destructor()
{
	syscall(THREAD_SYSCALLS_BASE+9);
	__builtin_unreachable();
}
