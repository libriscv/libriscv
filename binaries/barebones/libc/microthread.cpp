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
	__attribute__((constructor))
	void init_threads()
	{
		main_thread.tid = 0;
		asm("mv tp, %0" : : "r"(&main_thread) : "tp");
	}
}

extern "C"
__attribute__((noinline))
int threadcall_executor(...)
{
	return syscall(THREAD_SYSCALLS_BASE+8);
}

extern "C"
void threadcall_destructor()
{
	syscall(THREAD_SYSCALLS_BASE+9);
	__builtin_unreachable();
}
