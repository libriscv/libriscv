#pragma once
#include <cstdio>
#include <stdexcept>
#include <unordered_map>
#include <libriscv/machine.hpp>

namespace riscv {

template <int W> struct MultiThreading;
static const uint32_t PARENT_SETTID  = 0x00100000; /* set the TID in the parent */
static const uint32_t CHILD_CLEARTID = 0x00200000; /* clear the TID in the child */
static const uint32_t CHILD_SETTID   = 0x01000000; /* set the TID in the child */

//#define THREADS_DEBUG 1
#ifdef THREADS_DEBUG
#define THPRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define THPRINT(fmt, ...) /* fmt */
#endif

template <int W>
struct Thread
{
	using address_t = address_type<W>;

	MultiThreading<W>& threading;
	const int tid;
	// for returning to this thread
	Registers<W> stored_regs;
	// address zeroed when exiting
	address_t clear_tid = 0;
	// the current or last blocked reason
	int block_reason = 0;

	Thread(MultiThreading<W>&, int tid,
			address_t tls, address_t stack);
	Thread(MultiThreading<W>&, const Thread& other);
	bool exit(); // Returns false when we *cannot* continue
	void suspend();
	void suspend(address_t return_value);
	void block(int reason);
	void block(int reason, address_t return_value);
	void activate();
	void resume();
};

template <int W>
struct MultiThreading
{
	using address_t = address_type<W>;
	using thread_t  = Thread<W>;

	thread_t* create(int flags, address_t ctid, address_t ptid,
					address_t stack, address_t tls);
	int       get_tid() const { return m_current->tid; }
	thread_t* get_thread();
	thread_t* get_thread(int tid); /* or nullptr */
	bool      suspend_and_yield();
	bool      yield_to(int tid, bool store_retval = true);
	void      erase_thread(int tid);
	void      wakeup_next();
	bool      block(int reason);
	void      unblock(int tid);
	bool      wakeup_blocked(int reason);

	MultiThreading(Machine<W>&);
	MultiThreading(Machine<W>&, const MultiThreading&);
	Machine<W>& machine;
	std::vector<thread_t*> m_blocked;
	std::vector<thread_t*> m_suspended;
	std::unordered_map<int, thread_t> m_threads;
	int        thread_counter = 0;
	thread_t*  m_current = nullptr;
};

/** Implementation **/

template <int W>
inline MultiThreading<W>::MultiThreading(Machine<W>& mach)
	: machine(mach)
{
	// Create the main thread
	auto it = m_threads.try_emplace(0, *this, 0, 0x0, mach.cpu.reg(REG_SP));
	m_current = &it.first->second;
}

template <int W>
inline MultiThreading<W>::MultiThreading(Machine<W>& mach, const MultiThreading<W>& other)
	: machine(mach)
{
	for (const auto& it : other.m_threads) {
		const int tid = it.first;
		m_threads.try_emplace(tid, *this, it.second);
	}
	/* Copy each suspended by pointer lookup */
	m_suspended.reserve(other.m_suspended.size());
	for (const auto* t : other.m_suspended) {
		m_suspended.push_back(get_thread(t->tid));
	}
	/* Copy each blocked by pointer lookup */
	m_blocked.reserve(other.m_blocked.size());
	for (const auto* t : other.m_blocked) {
		m_blocked.push_back(get_thread(t->tid));
	}
	/* Copy current thread */
	m_current = get_thread(other.m_current->tid);
}

template <int W>
inline void Thread<W>::resume()
{
	threading.m_current = this;
	auto& m = threading.machine;
	// restore registers
	m.cpu.registers() = this->stored_regs;
	THPRINT("Returning to tid=%ld tls=0x%X stack=0x%X\n",
			this->tid,
			this->stored_regs.get(REG_TP),
			this->stored_regs.get(REG_SP));
	// this will ensure PC is executable in all cases
	m.cpu.aligned_jump(m.cpu.pc());
}

template <int W>
inline void Thread<W>::suspend()
{
	this->stored_regs = threading.machine.cpu.registers();
	// add to suspended (NB: can throw)
	threading.m_suspended.push_back(this);
}

template <int W>
inline void Thread<W>::suspend(address_t return_value)
{
	this->suspend();
	// set the *future* return value for this thread
	this->stored_regs.get(REG_ARG0) = return_value;
}

template <int W>
inline void Thread<W>::block(int reason)
{
	this->stored_regs = threading.machine.cpu.registers();
	this->block_reason = reason;
	// add to blocked (NB: can throw)
	threading.m_blocked.push_back(this);
}

template <int W>
inline void Thread<W>::block(int reason, address_t return_value)
{
	this->block(reason);
	// set the block reason as the next return value
	this->stored_regs.get(REG_ARG0) = return_value;
}

template <int W>
inline Thread<W>* MultiThreading<W>::get_thread()
{
	return this->m_current;
}

template <int W>
inline Thread<W>* MultiThreading<W>::get_thread(int tid)
{
	auto it = m_threads.find(tid);
	if (it == m_threads.end()) return nullptr;
	return &it->second;
}

template <int W>
inline void MultiThreading<W>::wakeup_next()
{
	// resume a waiting thread
	assert(!m_suspended.empty());
	auto* next = m_suspended.front();
	m_suspended.erase(m_suspended.begin());
	// resume next thread
	next->resume();
}

template <int W>
inline Thread<W>::Thread(
	MultiThreading<W>& mt, int ttid, address_t tls, address_t stack)
	: threading(mt), tid(ttid)
{
	this->stored_regs.get(REG_TP) = tls;
	this->stored_regs.get(REG_SP) = stack;
}

template <int W>
inline Thread<W>::Thread(
	MultiThreading<W>& mt, const Thread& other)
	: threading(mt), tid(other.tid), stored_regs(other.stored_regs),
	  clear_tid(other.clear_tid), block_reason(other.block_reason)
{}

template <int W>
inline void Thread<W>::activate()
{
	threading.m_current = this;
	auto& cpu = threading.machine.cpu;
	cpu.reg(REG_TP) = this->stored_regs.get(REG_TP);
	cpu.reg(REG_SP) = this->stored_regs.get(REG_SP);
}

template <int W>
inline bool Thread<W>::exit()
{
	const bool exiting_myself = (threading.get_thread() == this);
	// Copy of reference to thread manager and thread ID
	auto& thr = this->threading;
	const int tid = this->tid;
	// CLONE_CHILD_CLEARTID: set userspace TID value to zero
	if (this->clear_tid) {
		THPRINT("Clearing thread value for tid=%d at 0x%X\n",
				this->tid, this->clear_tid);
		threading.machine.memory.
			template write<address_type<W>> (this->clear_tid, 0);
	}
	// Delete this thread (except main thread)
	if (tid != 0) {
		threading.erase_thread(tid);

		// Resume next thread in suspended list
		// Exiting main thread is a "process exit", so we don't wakeup_next
		if (exiting_myself) {
			thr.wakeup_next();
		}
	}

	// tid == 0: Main thread exited
	return (tid == 0);
}

template <int W>
inline Thread<W>* MultiThreading<W>::create(
			int flags, address_t ctid, address_t ptid,
			address_t stack, address_t tls)
{
	const int tid = ++this->thread_counter;
	auto it = m_threads.emplace(tid, Thread{*this, tid, tls, stack});
	auto* thread = &it.first->second;

	// flag for write child TID
	if (flags & CHILD_SETTID) {
		machine.memory.template write<uint32_t> (ctid, thread->tid);
	}
	if (flags & PARENT_SETTID) {
		machine.memory.template write<uint32_t> (ptid, thread->tid);
	}
	if (flags & CHILD_CLEARTID) {
		thread->clear_tid = ctid;
	}

	return thread;
}

template <int W>
inline bool MultiThreading<W>::suspend_and_yield()
{
	auto* thread = get_thread();
	// don't go through the ardous yielding process when alone
	if (m_suspended.empty()) {
		// set the return value for sched_yield
		machine.cpu.reg(REG_ARG0) = 0;
		return false;
	}
	// suspend current thread, and return 0 when resumed
	thread->suspend(0);
	// resume some other thread
	this->wakeup_next();
	return true;
}

template <int W>
inline bool MultiThreading<W>::block(int reason)
{
	auto* thread = get_thread();
	if (UNLIKELY(m_suspended.empty())) {
		// TODO: Stop the machine here?
		return false; // continue immediately?
	}
	// block thread, write reason to future return value
	thread->block(reason, reason);
	// resume some other thread
	this->wakeup_next();
	return true;
}

template <int W>
inline bool MultiThreading<W>::yield_to(int tid, bool store_retval)
{
	auto* thread = get_thread();
	auto* next   = get_thread(tid);
	if (next == nullptr) {
		if (store_retval) machine.cpu.reg(REG_ARG0) = -1;
		return false;
	}
	if (thread == next) {
		// immediately returning back to caller
		if (store_retval) machine.cpu.reg(REG_ARG0) = 0;
		return false;
	}
	// suspend current thread
	if (store_retval)
		thread->suspend(0);
	else
		thread->suspend();
	// remove the next thread from suspension
	for (auto it = m_suspended.begin(); it != m_suspended.end(); ++it) {
		if (*it == next) {
			m_suspended.erase(it);
			break;
		}
	}
	// resume next thread
	next->resume();
	return true;
}

template <int W>
inline void MultiThreading<W>::unblock(int tid)
{
	for (auto it = m_blocked.begin(); it != m_blocked.end(); )
	{
		if ((*it)->tid == tid)
		{
			// suspend current thread
			get_thread()->suspend(0);
			// resume this thread
			(*it)->resume();
			m_blocked.erase(it);
			return;
		}
		else ++it;
	}
	// given thread id was not blocked
	machine.cpu.reg(REG_ARG0) = -1;
}
template <int W>
inline bool MultiThreading<W>::wakeup_blocked(int reason)
{
	for (auto it = m_blocked.begin(); it != m_blocked.end(); )
	{
		// compare against block reason
		if ((*it)->block_reason == reason)
		{
			// suspend current thread
			get_thread()->suspend(0);
			// resume this thread
			(*it)->resume();
			m_blocked.erase(it);
			return true;
		}
		else ++it;
	}
	// nothing to wake up
	return false;
}

template <int W>
inline void MultiThreading<W>::erase_thread(int tid)
{
	auto it = m_threads.find(tid);
	assert(it != m_threads.end());
	m_threads.erase(it);
}

} // riscv
