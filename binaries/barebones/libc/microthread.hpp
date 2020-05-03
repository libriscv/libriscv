#pragma once
#include "include/common.hpp"
#include "include/syscall.hpp"
#include <functional>
#include <memory>

/***
 * Example usage:
 *
 *	auto thread = microthread::create(
 *		[] (int a, int b, int c) -> long {
 *			printf("Hello from a microthread!\n"
 *					"a = %d, b = %d, c = %d\n",
 *					a, b, c);
 *			return a + b + c;
 *		}, 111, 222, 333);
 *
 *  long retval = microthread::join(thread);
 *  printf("microthread exit status: %ld\n", retval);
 *
 *  Note: microthreads require the native threads system calls.
 *  microthreads do not support thread-local storage.
 *  The thread function may also return void, in which case the
 *  return value becomes zero (0).
***/

namespace microthread
{
struct Thread;

/* Create a new thread using the given function @func,
   and pass all further arguments to the function as is.
   Returns the new thread. The new thread starts immediately. */
template <typename T, typename... Args>
auto    create(const T& func, Args&&... args);

/* Create a new self-governing thread that deletes itself on completion.
   Calling exit() in a sovereign thread is undefined behavior. 
   Returns thread id on success. */
template <typename T, typename... Args>
int     oneshot(const T& func, Args&&... args);

/* Create a new self-governing thread that directly starts on the
   threads start function, with a special return address that
   self-deletes and exits the thread safely. Data can be retrieved
   using microthread::getdata(). Returns thread id on success. */
int     direct(void(*) (), void* data = nullptr);

/* Waits for a thread to finish and then returns the exit status
   of the thread. The thread is then deleted, freeing memory. */
long    join(Thread*);

/* Exit the current thread with the given exit status. Never returns. */
void    exit(long status);

/* Yield until condition is true */
void    yield_until(const std::function<bool()>& condition);
/* Return back to another suspended thread. Returns 0 on success. */
long    yield();
long    yield_to(int tid); /* Return to a specific suspended thread. */
long    yield_to(Thread*);

/* Block a thread with a specific reason. */
long    block(int reason);
void    block(int reason, const std::function<bool()>& condition);
long    unblock(int tid);
/* Wake thread with @reason that was blocked, returns -1 if nothing happened. */
long    wakeup_one_blocked(int reason);

Thread* self();            /* Returns the current thread */
int     gettid();          /* Returns the current thread id */


/** implementation details **/

struct Thread
{
	static const size_t STACK_SIZE = 256*1024;

	Thread(std::function<void()> start)
	 	: startfunc{std::move(start)} {}
	Thread(void(*func)(), void* data)
		: tinyfunc{func}, tinydata{data} {}

	long resume()   { return yield_to(this); }
	long suspend()  { return yield(); }
	void exit(long status);

	bool has_exited() const;

	~Thread() {}

	int   tid;
	union {
		long  return_value;
		std::function<void()> startfunc;
		struct {
			void (*tinyfunc) ();
			void* tinydata;
		};
	};
};
struct ThreadDeleter {
	void operator() (Thread* thread) {
		join(thread);
	}
};
static_assert(Thread::STACK_SIZE > sizeof(Thread) + 16384);
using Thread_ptr = std::unique_ptr<Thread, ThreadDeleter>;

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

inline void* getdata() {
	return self()->tinydata;
}

inline long clone_helper(long sp, long tls, long ctid)
{
	extern void trampoline(Thread*);
	/* stack, func, tls, flags */
	return syscall(500, sp, (long) &trampoline, tls, ctid);
}

template <typename T, typename... Args>
inline auto create(const T& func, Args&&... args)
{
	char* stack_bot = (char*) malloc(Thread::STACK_SIZE);
	if (stack_bot == nullptr) return Thread_ptr{};
	char* stack_top = stack_bot + Thread::STACK_SIZE;
	// store arguments on stack
	char* args_addr = stack_bot + sizeof(Thread);
	auto* tuple = new (args_addr) std::tuple{std::move(args)...};

	// store the thread at the beginning of the stack
	Thread* thread = new (stack_bot) Thread(
		[func, tuple] ()
		{
			if constexpr (std::is_same_v<void, decltype(func(args...))>)
			{
				std::apply(func, std::move(*tuple));
				self()->exit(0);
			} else {
				self()->exit( std::apply(func, std::move(*tuple)) );
			}
		});

	const long tls  = (long) thread;
	const long ctid = 0x80000000;

	(void) clone_helper((long) stack_top, tls, ctid);
	// parent path (reordering doesn't matter)
	return Thread_ptr(thread);
}
template <typename T, typename... Args>
inline int oneshot(const T& func, Args&&... args)
{
	static_assert(std::is_same_v<void, decltype(func(args...))>,
				"Free threads have no return value!");
	char* stack_bot = (char*) malloc(Thread::STACK_SIZE);
	if (UNLIKELY(stack_bot == nullptr)) return -ENOMEM;
	char* stack_top = stack_bot + Thread::STACK_SIZE;
	// store arguments on stack
	char* args_addr = stack_bot + sizeof(Thread);
	auto* tuple = new (args_addr) std::tuple{std::move(args)...};
	// store the thread at the beginning of the stack
	Thread* thread = new (stack_bot) Thread(
		[func, tuple] {
			std::apply(func, std::move(*tuple));
			extern void oneshot_exit();
			oneshot_exit();
		});
	const long tls  = (long) thread;
	return clone_helper((long) stack_top, tls, 0);
}
inline int direct(void(*func)(), void* data)
{
	char* stack_bot = (char*) malloc(Thread::STACK_SIZE);
	if (UNLIKELY(stack_bot == nullptr)) return -ENOMEM;
	char* stack_top = stack_bot + Thread::STACK_SIZE;
	// store the thread at the beginning of the stack
	Thread* thread = new (stack_bot) Thread(func, data);
	const long tls  = (long) thread;
	extern void direct_starter(Thread*);
	/* stack, func, tls, flags */
	return syscall(500, (long) stack_top, (long) &direct_starter, tls, 0);
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
inline long join(Thread_ptr& tp)
{
	return join(tp.release());
}

inline void yield_until(const std::function<bool()>& condition)
{
	do {
		yield();
		asm("" ::: "memory");
	} while (!condition());
}
inline long yield()
{
	return syscall(502);
}
inline long yield_to(int tid)
{
	return syscall(503, tid);
}
inline long yield_to(Thread* thread)
{
	return yield_to(thread->tid);
}

inline long block(int reason = 0)
{
	return syscall(504, reason);
}
inline void block(int reason, const std::function<bool()>& condition)
{
	while (!condition()) {
		if (block(reason) < 0) break;
		asm("" ::: "memory");
	}
}
inline long wakeup_one_blocked(int reason)
{
	return syscall(505, reason);
}
inline long unblock(int tid)
{
	return syscall(506, tid);
}

__attribute__((noreturn))
inline void exit(long exitcode)
{
	self()->exit(exitcode);
	__builtin_unreachable();
}

__attribute__((noreturn))
inline void Thread::exit(long exitcode)
{
	this->tid = 0;
	this->return_value = exitcode;
	syscall(501, exitcode);
	__builtin_unreachable();
}

}
