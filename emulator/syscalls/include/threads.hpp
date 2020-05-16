#pragma once
#include <EASTL/fixed_map.h>
#include <libriscv/machine.hpp>
#include "syscall_helpers.hpp"
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
	int       tid;
	address_t my_tls;
	address_t my_stack;
	// for returning to this thread
	riscv::Registers<W> stored_regs;
	// address zeroed when exiting
	address_t clear_tid = 0;

	thread(multithreading<W>&, int tid,
			address_t tls, address_t stack);
	void exit();
	void suspend(address_t return_value);
	void block(int reason);
	void activate();
	void resume();
};

template <int W>
struct multithreading
{
	using address_t = riscv::address_type<W>;
	using thread_t  = thread<W>;

	thread_t* create(int flags, address_t ctid, address_t ptid,
					address_t stack, address_t tls);
	thread_t* get_thread();
	thread_t* get_thread(int tid); /* or nullptr */
	bool      suspend_and_yield();
	void      yield_to(int tid);
	void      erase_thread(int tid);
	void      wakeup_next();
	bool      block(int reason);
	void      unblock(int tid);
	void      wakeup_blocked(int reason);

	multithreading(riscv::Machine<W>&);
	~multithreading();
	riscv::Machine<W>& machine;
	std::vector<thread_t*> blocked;
	std::vector<thread_t*> suspended;
	eastl::fixed_map<int, thread_t*, 32> threads;
	int        thread_counter = 0;
	thread_t*  m_current = nullptr;
	thread_t   main_thread;
};

template <int W>
void setup_multithreading(State<W>&, riscv::Machine<W>&);

template <int W>
void setup_native_threads(riscv::Machine<W>&);
