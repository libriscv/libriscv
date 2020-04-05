#pragma once
#include "include/syscall.hpp"
#include <functional>

namespace microthread
{
struct Thread;

template <typename T, typename... Args>
Thread* create(const T& func, Args&&... args);
Thread* self();
int     gettid();
long    join(Thread*);
long    yield();
long    yield_to(int tid);
long    yield_to(Thread*);
void    exit(long statuscode);

struct Thread
{
	static const size_t STACK_SIZE = 256*1024;

	Thread(std::function<void()> start)
	 	: startfunc{std::move(start)} {}

	long resume()   { return yield_to(this); }
	long suspend()  { return yield(); }

	bool has_exited() const noexcept {
		return this->tid == 0;
	}

	__attribute__((noreturn)) void exit(long rv);
	~Thread() {}

	int   tid;
	union {
		long  return_value;
		std::function<void()> startfunc;
	};
};
static_assert(Thread::STACK_SIZE > sizeof(Thread) + 16384);

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
