#include "microthread.hpp"
#include <cstdio>

namespace microthread
{
	void trampoline(Thread* thread)
	{
		thread->startfunc();
	}
}

namespace std {
	void __throw_bad_function_call() {
		printf("Bad function call detected!\n");
		abort();
	}
}
