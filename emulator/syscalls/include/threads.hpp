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
	const int tid;
	address_t my_tls;
	address_t my_stack;
	// for returning to this thread
	riscv::Registers<W> stored_regs;
	// address zeroed when exiting
	address_t clear_tid = 0;
	// the current or last blocked reason
	int block_reason = 0;

	thread(multithreading<W>&, int tid,
			address_t tls, address_t stack);
	void exit();
	void suspend();
	void suspend(address_t return_value);
	void block(int reason);
	void block(int reason, address_t return_value);
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
	bool      yield_to(int tid, bool store_retval = true);
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
multithreading<W>* setup_native_threads(riscv::Machine<W>&);



template <int W>
inline void thread<W>::resume()
{
	THPRINT("Returning to tid=%ld tls=%p stack=%p\n",
			this->tid, (void*) this->my_tls, (void*) this->my_stack);

	threading.m_current = this;
	auto& m = threading.machine;
	// preserve some registers
	auto counter = m.cpu.registers().counter;
	// restore registers
	m.cpu.registers() = this->stored_regs;
	m.cpu.registers().counter = counter;
}

template <int W>
inline void thread<W>::suspend()
{
	this->stored_regs = threading.machine.cpu.registers();
	// add to suspended (NB: can throw)
	threading.suspended.push_back(this);
}

template <int W>
inline void thread<W>::suspend(address_t return_value)
{
	this->suspend();
	// set the *future* return value for this thread
	this->stored_regs.get(riscv::RISCV::REG_ARG0) = return_value;
}

template <int W>
inline void thread<W>::block(int reason)
{
	this->stored_regs = threading.machine.cpu.registers();
	this->block_reason = reason;
	// add to blocked (NB: can throw)
	threading.blocked.push_back(this);
}

template <int W>
inline void thread<W>::block(int reason, address_t return_value)
{
	this->block(reason);
	// set the block reason as the next return value
	this->stored_regs.get(riscv::RISCV::REG_ARG0) = return_value;
}

template <int W>
inline thread<W>* multithreading<W>::get_thread()
{
	return this->m_current;
}

template <int W>
inline thread<W>* multithreading<W>::get_thread(int tid)
{
	auto it = threads.find(tid);
	if (it == threads.end()) return nullptr;
	return it->second;
}

template <int W>
inline void multithreading<W>::wakeup_next()
{
	// resume a waiting thread
	assert(!suspended.empty());
	auto* next = suspended.front();
	suspended.erase(suspended.begin());
	// resume next thread
	next->resume();
}
