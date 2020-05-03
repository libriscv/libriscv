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
		syscall(501, 0);
		__builtin_unreachable();
	}

	static Thread main_thread {nullptr};
	__attribute__((constructor))
	void init_threads()
	{
		main_thread.tid = 0;
		asm("mv tp, %0" : : "r"(&main_thread) : "tp");
	}
}
