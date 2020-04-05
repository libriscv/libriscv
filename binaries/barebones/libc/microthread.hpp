#pragma once
#include "include/syscall.hpp"
#include <functional>

namespace microthread
{
struct Thread;

template <typename T, typename... Args>
Thread* create(const T& func, Args&&... args);
Thread* self();
long    join(Thread*);
void    yield();
void    exit(long statuscode);

struct Thread
{
	Thread(std::function<void()> start)
	 	: startfunc{start}
	{
		static int counter = 0;
		tid = ++counter;
	}

	__attribute__((noreturn))
	void exit(long rv);

	int   tid;
	long  return_value;
	std::function<void()> startfunc;
};

inline Thread* self() {
	Thread* tp;
	asm("mv %0, tp" : "=r"(tp));
	return (Thread*) tp;
}

inline char* realign_stack(char* stack) {
	return (char*) ((long) stack & ~0xF);
}

inline long clone_helper(long sp, long args, long ctid)
{
	extern void trampoline(Thread*);
	/* stack, func, tls, ctid */
	return syscall(500, sp, (long) &trampoline, args, ctid);
}

template <typename T, typename... Args>
inline Thread* create(const T& func, Args&&... args)
{
	static const size_t STACK_SIZE = 1*1024*1024;
	char* stack_bot = new char[sizeof(Thread) + STACK_SIZE];
	char* stack_top = realign_stack(stack_bot + STACK_SIZE);

	// store the thread at the beginning of the stack
	auto* thread = new (stack_bot) Thread(
		[func, args...] () {
			self()->exit( func(args...) );
		});

	const long tls  = (long) thread;
	const long ctid = (long) &thread->tid;

	(void) clone_helper((long) stack_top, tls, ctid);
	// parent path (reordering doesn't matter)
	return thread;
}

inline long join(Thread* thread)
{
	// yield until the tid value is zeroed
	while (thread->tid != 0) {
		yield();
		// thread->tid might have changed since yielding
		asm ("" : : : "memory");
	}
	const long rv = thread->return_value;
	delete thread;
	return rv;
}

inline void yield()
{
	syscall(124, 0);
}

__attribute__((noreturn))
inline void exit(long exitcode)
{
	if (self() != nullptr) {
		self()->exit(exitcode);
		__builtin_unreachable();
	}
	abort();
}

inline void Thread::exit(long exitcode)
{
	this->tid = 0;
	this->return_value = exitcode;
	syscall(93, exitcode);
	__builtin_unreachable();
}

}
