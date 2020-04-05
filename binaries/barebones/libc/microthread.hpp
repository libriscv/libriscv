#pragma once
#include "include/syscall.hpp"
#include <functional>

/***
 * Example usage:
 *
 * 	auto* thread = microthread::create(
 *		[] (int a, int b, int c) -> long {
 *			printf("Hello from a microthread!\n"
 *					"a = %d, b = %d, c = %d\n",
 *					a, b, c);
 *      }, 111, 222, 333);
 *
 *  long retval = microthread::join(thread);
 *  printf("microthread exit status: %ld\n", retval);
 *
 *  Note: microthreads require the native threads system calls.
 *  microthreads do not support thread-local storage.
***/

namespace microthread
{
struct Thread;

/* Create a new thread using the given function @func,
   and pass all further arguments to the function as is.
   Returns the new thread. The new thread starts immediately. */
template <typename T, typename... Args>
Thread* create(const T& func, Args&&... args);

/* Waits for a thread to finish and then returns the exit status
   of the thread. The thread is then deleted, freeing memory. */
long    join(Thread*);

/* Exit the current thread with the given exit status. Never returns. */
void    exit(long status);

/* Return back to another suspended thread. Returns 0 on success. */
long    yield();
long    yield_to(int tid); /* Return to a specific suspended thread. */
long    yield_to(Thread*);

Thread* self();            /* Returns the current thread */
int     gettid();          /* Returns the current thread id */


/** implementation details **/

struct Thread
{
	static const size_t STACK_SIZE = 256*1024;

	Thread(std::function<void()> start)
	 	: startfunc{std::move(start)} {}

	long resume()   { return yield_to(this); }
	long suspend()  { return yield(); }
	void exit(long status);

	bool has_exited() const;

	~Thread() {}

	int   tid;
	union {
		long  return_value;
		std::function<void()> startfunc;
	};
};
static_assert(Thread::STACK_SIZE > sizeof(Thread) + 16384);

inline bool Thread::has_exited() const {
	return this->tid == 0;
}

inline Thread* self() {
	Thread* tp;
	asm("mv %0, tp" : "=r"(tp));
	return (Thread*) tp;
}

inline int gettid() {
	return self()->tid;
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
	char* stack_bot = (char*) malloc(Thread::STACK_SIZE);
	if (stack_bot == nullptr) return nullptr;
	char* stack_top = stack_bot + Thread::STACK_SIZE;

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
	while (!thread->has_exited()) {
		yield();
		// thread->tid might have changed since yielding
		asm ("" : : : "memory");
	}
	const long rv = thread->return_value;
	free(thread);
	return rv;
}

inline long yield()
{
	return syscall(124, 0);
}
inline long yield_to(int tid)
{
	return syscall(501, tid);
}
inline long yield_to(Thread* thread)
{
	return yield_to(thread->tid);
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

__attribute__((noreturn))
inline void Thread::exit(long exitcode)
{
	this->tid = 0;
	this->return_value = exitcode;
	syscall(93, exitcode);
	__builtin_unreachable();
}

}
