#pragma once
#include <include/common.hpp>
#include <include/syscall.hpp>
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
   self-deletes and exits the thread safely. Arguments can be passed,
   but they must be used by value, unless you know what you are doing.
   Returns thread id on success. The first argument is the Thread&. */
#ifdef USE_THREADCALLS
template <typename T, typename... Args>
int     direct(const T& func, Args&&... args);

#else
int     direct(void (*func) (), void* data = nullptr);

#endif

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
long    block(int reason = 0);
void    block(const std::function<bool()>& condition, int reason = 0);
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

inline long clone_helper(long sp, long tls)
{
	extern void trampoline(Thread*);
	asm("" ::: "memory"); /* avoid dead-store optimization */
	/* stack, func, tls */
	return syscall(THREAD_SYSCALLS_BASE+0, sp, (long) &trampoline, tls);
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
		[func, tuple] () -> void
		{
			if constexpr (std::is_same_v<void, decltype(func(args...))>)
			{
				std::apply(func, std::move(*tuple));
				self()->exit(0);
			} else {
				self()->exit( std::apply(func, std::move(*tuple)) );
			}
		});

	(void) clone_helper((long) stack_top, (long) thread);
	// parent path (reordering doesn't matter)
	return Thread_ptr(thread);
}
template <typename T, typename... Args>
inline int oneshot(const T& func, Args&&... args)
{
	static_assert(std::is_same_v<void, decltype(func(args...))>,
				"Free threads have no return value!");
	char* stack_bot = (char*) malloc(Thread::STACK_SIZE);
	if (UNLIKELY(stack_bot == nullptr)) return -12; /* ENOMEM */
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
	return clone_helper((long) stack_top, tls);
}

template <typename Ret = long, typename... Args>
inline Ret threadcall(Args&&... args);

#ifdef USE_THREADCALLS
template <typename T, typename... Args>
inline int direct(const T& func, Args&&... args)
{
	auto fptr = static_cast<void(*)(Thread&, Args...)> (func);
	return threadcall(fptr, std::forward<Args> (args)...);
}
#else
inline int direct(void (*func) (), void* data)
{
	extern void direct_starter(Thread*);
	char* stack_bot = (char*) malloc(Thread::STACK_SIZE);
	if (UNLIKELY(stack_bot == nullptr)) return -12; /* ENOMEM */
	char* stack_top = stack_bot + Thread::STACK_SIZE;
	// store the thread at the beginning of the stack
	Thread* thread = new (stack_bot) Thread(func, data);
	asm("" ::: "memory");
	const long tls  = (long) thread;
	/* stack, func, tls, flags */
	return syscall(THREAD_SYSCALLS_BASE+0, (long) stack_top, (long) &direct_starter, tls, 0);
}
#endif

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
	return syscall(THREAD_SYSCALLS_BASE+2);
}
inline long yield_to(int tid)
{
	return syscall(THREAD_SYSCALLS_BASE+3, tid);
}
inline long yield_to(Thread* thread)
{
	return yield_to(thread->tid);
}

inline long block(int reason)
{
	return syscall(THREAD_SYSCALLS_BASE+4, reason);
}
inline void block(const std::function<bool()>& condition, int reason)
{
	while (!condition()) {
		if (block(reason) < 0) break;
		asm("" ::: "memory");
	}
}
inline long wakeup_one_blocked(int reason)
{
	return syscall(THREAD_SYSCALLS_BASE+5, reason);
}
inline long unblock(int tid)
{
	return syscall(THREAD_SYSCALLS_BASE+6, tid);
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
	syscall(THREAD_SYSCALLS_BASE+1, exitcode);
	__builtin_unreachable();
}


/** For when USE_THREADCALLS is enabled **/

template <typename Ret = long, typename... Args>
inline Ret threadcall(Args&&... args)
{
	using tcall_t = Ret (*) (...);
	// Special thread system call number is offset / 4
	return ((tcall_t) (0xFFFFE000)) (std::forward<Args>(args)...);
}

}
