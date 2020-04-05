#include "threads.hpp"
#include <cassert>
#include <cstdio>
#include <sched.h>
using namespace riscv;

template <int W>
thread<W>::thread(multithreading<W>& mt, int ttid, thread* p,
				address_t tls, address_t stack)
	: threading(mt), parent(p), tid(ttid), my_tls(tls), my_stack(stack)   {}

template <int W>
void thread<W>::activate()
{
	threading.m_current = this;
	auto& cpu = threading.machine.cpu;
	cpu.reg(RISCV::REG_SP) = this->my_stack;
	cpu.reg(RISCV::REG_TP) = this->my_tls;
}

template <int W>
void thread<W>::suspend()
{
	this->stored_regs = threading.machine.cpu.registers();
	// add to suspended (NB: can throw)
	threading.suspended.push_back(this);
}

template <int W>
void thread<W>::exit()
{
	const bool exiting_myself = (threading.get_thread() == this);
	assert(this->parent != nullptr);
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
	// free thread resources
	delete this;

	if (exiting_myself)
	{
		// resume next thread in suspended list
		thr.wakeup_next();
	}
}

template <int W>
void thread<W>::resume()
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
thread<W>* multithreading<W>::create(
			thread_t* parent, int flags, address_t ctid, address_t ptid,
			address_t stack, address_t tls)
{
	const int tid = ++thread_counter;
	auto* thread = new thread_t(*this, tid, parent, tls, stack);

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

	threads.emplace(
		std::piecewise_construct,
		std::forward_as_tuple(tid),
		std::forward_as_tuple(thread));
	return thread;
}

template <int W>
multithreading<W>::multithreading(Machine<W>& mach)
	: machine(mach), main_thread(*this, 0, nullptr, 0x0, 0x0)
{
	main_thread.my_stack = machine.cpu.reg(RISCV::REG_SP);
	m_current = &main_thread;
}

template <int W>
thread<W>* multithreading<W>::get_thread()
{
	return this->m_current;
}

template <int W>
thread<W>* multithreading<W>::get_thread(int tid)
{
	auto it = threads.find(tid);
	if (it == threads.end()) return nullptr;
	return it->second;
}

template <int W>
bool multithreading<W>::suspend_and_yield()
{
	auto* thread = get_thread();
	// don't go through the ardous yielding process when alone
	if (suspended.empty()) {
		// set the return value for sched_yield
		thread->stored_regs.get(RISCV::REG_ARG0) = 0;
		return false;
	}
	// suspend current thread
	thread->suspend();
	// set the return value for sched_yield
	thread->stored_regs.get(RISCV::REG_ARG0) = 0;
	// resume some other thread
	this->wakeup_next();
	return true;
}

template <int W>
void multithreading<W>::wakeup_next()
{
  // resume a waiting thread
  assert(!suspended.empty());
  auto* next = suspended.front();
  suspended.pop_front();
  // resume next thread
  next->resume();
}

template <int W>
void multithreading<W>::erase_thread(int tid)
{
	auto it = threads.find(tid);
	assert(it != threads.end());
	threads.erase(it);
}

template <int W>
void setup_multithreading(State<W>& state, Machine<W>& machine)
{
	auto* mt = new multithreading<W>(machine);
	machine.add_destructor_callback([mt] { delete mt; });

	// exit & exit_group
	machine.install_syscall_handler(93,
	[mt, &state] (Machine<W>& machine) {
		const uint32_t status = machine.template sysarg<uint32_t> (0);
		const int tid = mt->get_thread()->tid;
		THPRINT(">>> Exit on tid=%ld, exit code = %d\n",
				tid, (int) status);
		if (tid != 0) {
			// exit thread instead
			mt->get_thread()->exit();
			// should be a new thread now
			assert(mt->get_thread()->tid != tid);
			return machine.cpu.reg(RISCV::REG_ARG0);
		}
		state.exit_code = status;
		machine.stop();
		return status;
	});
	// exit_group
	machine.install_syscall_handler(94, machine.get_syscall_handler(93));
	// set_tid_address
	machine.install_syscall_handler(96,
	[mt] (Machine<W>& machine) {
		const int clear_tid = machine.template sysarg<address_type<W>> (0);
		THPRINT(">>> set_tid_address(0x%X)\n", clear_tid);

		mt->get_thread()->clear_tid = clear_tid;
		return mt->get_thread()->tid;
	});
	// set_robust_list
	machine.install_syscall_handler(99,
	[] (Machine<W>&) {
		return 0;
	});
	// sched_yield
	machine.install_syscall_handler(124,
	[mt] (Machine<W>& machine) {
		THPRINT(">>> sched_yield()\n");
		// begone!
		mt->suspend_and_yield();
		// preserve A0 for the new thread
		return machine.cpu.reg(RISCV::REG_ARG0);
	});
	// tgkill
	machine.install_syscall_handler(131,
	[mt] (Machine<W>& machine) {
		const int tid = machine.template sysarg<int> (1);
		THPRINT(">>> tgkill on tid=%d\n", tid);
		auto* thread = mt->get_thread(tid);
		if (thread != nullptr) {
			// exit thread instead
			thread->exit();
			// preserve A0
			return machine.cpu.reg(RISCV::REG_ARG0);
		}
		machine.stop();
		return 0u;
	});
	// gettid
	machine.install_syscall_handler(178,
	[mt] (Machine<W>&) {
		THPRINT(">>> gettid() = %ld\n", mt->get_thread()->tid);
		return mt->get_thread()->tid;
	});
	// futex
	machine.install_syscall_handler(98,
	[mt] (Machine<W>& machine) {
		#define FUTEX_WAIT 0
		#define FUTEX_WAKE 1
		const uint32_t addr = machine.template sysarg<uint32_t> (0);
		const int  futex_op = machine.template sysarg<int> (1);
		const int       val = machine.template sysarg<int> (2);
		THPRINT(">>> futex(0x%X, op=%d, val=%d)\n", addr, futex_op, val);
		if ((futex_op & 0xF) == FUTEX_WAIT)
	    {
			THPRINT("FUTEX: Waiting for unlock... uaddr=0x%X val=%d\n", addr, val);
			while (machine.memory.template read<uint32_t> (addr) == val) {
				if (mt->suspend_and_yield()) {
					return (int) machine.cpu.reg(RISCV::REG_ARG0);
				}
				machine.cpu.trigger_exception(DEADLOCK_REACHED);
			}
			return 0;
		} else if ((futex_op & 0xF) == FUTEX_WAKE) {
			THPRINT("FUTEX: Waking others on %d\n", val);
			if (mt->suspend_and_yield()) {
				return (int) machine.cpu.reg(RISCV::REG_ARG0);
			}
			return 0;
		}
		return -ENOSYS;
	});
	// clone
	machine.install_syscall_handler(220,
	[mt] (Machine<W>& machine) {
		/* int clone(int (*fn)(void *arg), void *child_stack, int flags, void *arg,
		             void *parent_tidptr, void *tls, void *child_tidptr) */
		const int      flags = machine.template sysarg<int> (0);
		const uint32_t stack = machine.template sysarg<uint32_t> (1);
#ifdef THREADS_DEBUG
		const uint32_t  func = machine.template sysarg<uint32_t> (2);
		const uint32_t  args = machine.template sysarg<uint32_t> (3);
#endif
		const uint32_t  ptid = machine.template sysarg<uint32_t> (4);
		const uint32_t   tls = machine.template sysarg<uint32_t> (5);
		const uint32_t  ctid = machine.template sysarg<uint32_t> (6);
		auto* parent = mt->get_thread();
		THPRINT(">>> clone(func=0x%X, stack=0x%X, flags=%x, args=0x%X,"
				" parent=%p, ctid=0x%X ptid=0x%X, tls=0x%X)\n",
				func, stack, flags, args, parent, ctid, ptid, tls);
		auto* thread = mt->create(parent, flags, ctid, ptid, stack, tls);
		parent->suspend();
		// store return value for parent: child TID
		parent->stored_regs.get(RISCV::REG_ARG0) = thread->tid;
		// activate and return 0 for the child
		thread->activate();
		return 0;
	});
	// 500: microclone
	machine.install_syscall_handler(500,
	[mt] (Machine<W>& machine) {
		const uint32_t stack = machine.template sysarg<uint32_t> (0);
		const uint32_t  func = machine.template sysarg<uint32_t> (1);
		const uint32_t   tls = machine.template sysarg<uint32_t> (2);
		const uint32_t  ctid = machine.template sysarg<uint32_t> (3);
		auto* parent = mt->get_thread();
		auto* thread = mt->create(parent, CLONE_CHILD_CLEARTID, ctid, 0x0, stack, tls);
		parent->suspend();
		// store return value for parent: child TID
		parent->stored_regs.get(RISCV::REG_ARG0) = thread->tid;
		// activate and setup a function call
		thread->activate();
		// the cast is a work-around for a compiler bug
		machine.setup_call(func, (const uint32_t) tls);
		// preserve A0 for the new child thread
		return machine.cpu.reg(RISCV::REG_ARG0);
	});
}

template
void setup_multithreading<4>(State<4>&, Machine<4>& machine);
