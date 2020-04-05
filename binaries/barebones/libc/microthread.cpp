#include "microthread.hpp"
#include <cstdio>

namespace microthread
{
	void trampoline(Thread* thread)
	{
		thread->startfunc();
	}
	static Thread main_thread {nullptr};
	__attribute__((constructor))
	void init_threads()
	{
		main_thread.tid = 0;
		asm("mv tp, %0" : : "r"(&main_thread));
	}
}

namespace std {
	void __throw_bad_function_call() {
		printf("Bad function call detected!\n");
		abort();
	}
}
