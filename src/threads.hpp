#pragma once
#include <deque>
#include <map>
#include <vector>
#include <libriscv/machine.hpp>
template <int W> struct multithreading;

//#define THREADS_DEBUG 1
#ifdef THREADS_DEBUG
#define THPRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define THPRINT(fmt, ...) /* fmt */
#endif

template <int W>
struct thread
{
	using address_t = riscv::address_type<W>;

	multithreading<W>& threading;
	thread*   parent = nullptr;
	int64_t   tid;
	address_t my_tls;
	address_t my_stack;
	// for returning to this thread
	riscv::Registers<W> stored_regs;
	// address zeroed when exiting
	address_t clear_tid = 0;
	// children, detached when exited
	std::vector<thread*> children;

	thread(multithreading<W>&, int tid, address_t stack);
	void yield();
	void exit();
	void suspend();
	void activate(address_t func, address_t args);
	void resume();
};

template <int W>
struct multithreading
{
	using address_t = riscv::address_type<W>;
	using thread_t  = thread<W>;

	thread_t* create(thread_t* parent, int flags, address_t ctid,
					address_t stack, address_t tls);
	thread_t* get_thread();
	thread_t* get_thread(int64_t tid); /* or nullptr */
	void      suspend_and_yield();
	void      erase_suspension(thread_t*);
	void      erase_thread(int64_t tid);

	multithreading(riscv::Machine<W>&);
	riscv::Machine<W>& machine;
	std::deque<thread_t*> suspended;
    std::map<int64_t, thread_t*> threads;
	int64_t    thread_counter = 1;
	thread_t*  m_current = nullptr;
    thread_t   main_thread;
};

template <int W>
void setup_multithreading(riscv::Machine<W>&);
