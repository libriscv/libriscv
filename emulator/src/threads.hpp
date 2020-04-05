#pragma once
#include <deque>
#include <map>
#include <libriscv/machine.hpp>
#include "syscalls.hpp"
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
	int       tid;
	address_t my_tls;
	address_t my_stack;
	// for returning to this thread
	riscv::Registers<W> stored_regs;
	// address zeroed when exiting
	address_t clear_tid = 0;

	thread(multithreading<W>&, int tid, thread* parent,
			address_t tls, address_t stack);
	void exit();
	void suspend();
	void activate();
	void resume();
};

template <int W>
struct multithreading
{
	using address_t = riscv::address_type<W>;
	using thread_t  = thread<W>;

	thread_t* create(thread_t* parent, int flags, address_t ctid, address_t ptid,
					address_t stack, address_t tls);
	thread_t* get_thread();
	thread_t* get_thread(int tid); /* or nullptr */
	bool      suspend_and_yield();
	void      yield_to(int tid);
	void      erase_thread(int tid);
	void      wakeup_next();

	multithreading(riscv::Machine<W>&);
	riscv::Machine<W>& machine;
	std::deque<thread_t*> suspended;
	std::map<int, thread_t*> threads;
	int        thread_counter = 0;
	thread_t*  m_current = nullptr;
	thread_t   main_thread;
};

template <int W>
void setup_multithreading(State<W>&, riscv::Machine<W>&);
template <int W>
void setup_native_threads(State<W>&, riscv::Machine<W>&);
