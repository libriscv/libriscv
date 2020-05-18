#include <include/threads.hpp>
#include <cassert>
#include <cstdio>
#include <stdexcept>
#define CLONE_PARENT_SETTID   0x00100000 /* set the TID in the parent */
#define CLONE_CHILD_CLEARTID  0x00200000 /* clear the TID in the child */
#define CLONE_CHILD_SETTID    0x01000000 /* set the TID in the child */
using namespace riscv;

template <int W>
thread<W>::thread(multithreading<W>& mt, int ttid,
				address_t tls, address_t stack)
	: threading(mt), tid(ttid), my_tls(tls), my_stack(stack)   {}

template <int W>
void thread<W>::activate()
{
	threading.m_current = this;
	auto& cpu = threading.machine.cpu;
	cpu.reg(RISCV::REG_SP) = this->my_stack;
	cpu.reg(RISCV::REG_TP) = this->my_tls;
}

template <int W>
void thread<W>::exit()
{
	const bool exiting_myself = (threading.get_thread() == this);
	// temporary copy of thread manager
	auto& thr  = this->threading;
	// CLONE_CHILD_CLEARTID: set userspace TID value to zero
	if (this->clear_tid) {
		THPRINT("Clearing thread value for tid=%d at 0x%X\n",
				this->tid, this->clear_tid);
		threading.machine.memory.template write<uint32_t> (this->clear_tid, 0);
	}
	// delete this thread
	threading.erase_thread(this->tid);

	if (exiting_myself)
	{
		// resume next thread in suspended list
		thr.wakeup_next();
	}
}

template <int W>
thread<W>* multithreading<W>::create(
			int flags, address_t ctid, address_t ptid,
			address_t stack, address_t tls)
{
	const int tid = ++thread_counter;
	auto it = threads.emplace(tid, thread_t{*this, tid, tls, stack});
	thread_t* thread = &it.first->second;

	// flag for write child TID
	if (flags & CLONE_CHILD_SETTID) {
		machine.memory.template write<uint32_t> (ctid, thread->tid);
	}
	if (flags & CLONE_PARENT_SETTID) {
		machine.memory.template write<uint32_t> (ptid, thread->tid);
	}
	if (flags & CLONE_CHILD_CLEARTID) {
		thread->clear_tid = ctid;
	}

	return thread;
}

template <int W>
multithreading<W>::multithreading(Machine<W>& mach)
	: machine(mach), main_thread(*this, 0, 0x0, 0x0)
{
	main_thread.my_stack = machine.cpu.reg(RISCV::REG_SP);
	m_current = &main_thread;
}
template <int W>
multithreading<W>::~multithreading()
{
}

template <int W>
bool multithreading<W>::suspend_and_yield()
{
	auto* thread = get_thread();
	// don't go through the ardous yielding process when alone
	if (suspended.empty()) {
		// set the return value for sched_yield
		machine.cpu.reg(RISCV::REG_ARG0) = 0;
		return false;
	}
	// suspend current thread, and return 0 when resumed
	thread->suspend(0);
	// resume some other thread
	this->wakeup_next();
	return true;
}

template <int W>
bool multithreading<W>::block(int reason)
{
	auto* thread = get_thread();
	if (UNLIKELY(suspended.empty())) {
		// TODO: Stop the machine here?
		throw std::runtime_error("A blocked thread has nothing to yield to!");
	}
	// block thread, write reason to future return value
	thread->block(reason, reason);
	// resume some other thread
	this->wakeup_next();
	return true;
}

template <int W>
bool multithreading<W>::yield_to(int tid, bool store_retval)
{
	auto* thread = get_thread();
	auto* next   = get_thread(tid);
	if (next == nullptr) {
		if (store_retval) machine.cpu.reg(RISCV::REG_ARG0) = -1;
		return false;
	}
	if (thread == next) {
		// immediately returning back to caller
		if (store_retval) machine.cpu.reg(RISCV::REG_ARG0) = 0;
		return false;
	}
	// suspend current thread
	if (store_retval)
		thread->suspend(0);
	else
		thread->suspend();
	// remove the next thread from suspension
	for (auto it = suspended.begin(); it != suspended.end(); ++it) {
		if (*it == next) {
			suspended.erase(it);
			break;
		}
	}
	// resume next thread
	next->resume();
	return true;
}

template <int W>
void multithreading<W>::unblock(int tid)
{
	for (auto it = blocked.begin(); it != blocked.end(); )
	{
		if ((*it)->tid == tid)
		{
			// suspend current thread
			get_thread()->suspend(0);
			// resume this thread
			(*it)->resume();
			blocked.erase(it);
			return;
		}
		else ++it;
	}
	// given thread id was not blocked
	machine.cpu.reg(RISCV::REG_ARG0) = -1;
}
template <int W>
void multithreading<W>::wakeup_blocked(int reason)
{
	for (auto it = blocked.begin(); it != blocked.end(); )
	{
		// compare against block reason
		if ((*it)->block_reason == reason)
		{
			// suspend current thread
			get_thread()->suspend(0);
			// resume this thread
			(*it)->resume();
			blocked.erase(it);
			return;
		}
		else ++it;
	}
	// nothing to wake up
	machine.cpu.reg(RISCV::REG_ARG0) = -1;
}

template <int W>
void multithreading<W>::erase_thread(int tid)
{
	auto it = threads.find(tid);
	assert(it != threads.end());
	threads.erase(it);
}
