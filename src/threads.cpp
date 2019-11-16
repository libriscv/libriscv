#include "threads.hpp"
#include <cassert>
#include <cstdio>
using namespace riscv;

template <int W>
thread<W>::thread(multithreading<W>& mt, int ttid, address_t stack)
	: threading(mt), tid(ttid), my_stack(stack)   {}

template <int W>
void thread<W>::activate(address_t func, address_t args)
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
void thread<W>::yield()
{
  // resume a waiting thread
  assert(!threading.suspended.empty());
  auto* next = threading.suspended.front();
  threading.suspended.pop_front();
  // resume next thread
  next->resume();
}

template <int W>
void thread<W>::exit()
{
	const bool exiting_myself = (threading.get_thread() == this);
	assert(this->parent != nullptr);
	// detach children
	for (auto* child : this->children) {
		child->parent = &threading.main_thread;
	}
	// remove myself from parent
	auto& pcvec = this->parent->children;
	for (auto it = pcvec.begin(); it != pcvec.end(); ++it) {
		if (*it == this) {
			pcvec.erase(it); break;
		}
	}
	// temporary copy of parent thread pointer
	auto* next = this->parent;
	auto& thr  = threading;
	// CLONE_CHILD_CLEARTID: set userspace TID value to zero
	if (this->clear_tid) {
		THPRINT("Clearing child value at %p\n", this->clear_tid);
		threading.machine.memory.template write<address_t> (this->clear_tid, 0);
	}
	// delete this thread
	threading.erase_thread(this->tid);
	// free thread resources
	delete this;
	// resume parent thread
	if (exiting_myself)
	{
		thr.erase_suspension(next);
		next->resume();
	}
}

template <int W>
void thread<W>::resume()
{
	THPRINT("Returning to tid=%ld tls=%p nexti=%p stack=%p\n",
			this->tid, (void*) this->my_tls,
			(void*) this->stored_nexti, (void*) this->stored_stack);

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
			thread_t* parent, int flags, address_t ctid,
			address_t stack, address_t tls)
{
	const int tid = __sync_fetch_and_add(&thread_counter, 1);
	try {
		auto* thread = new thread_t(*this, tid, stack);
		thread->my_tls = tls;
		thread->parent = parent;
		thread->parent->children.push_back(thread);

		// flag for write child TID
		if (flags & CLONE_CHILD_SETTID) {
			machine.memory.template write<address_t> (ctid, thread->tid);
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
	catch (...) {
		return nullptr;
	}
}

template <int W>
multithreading<W>::multithreading(Machine<W>& mach)
	: machine(mach), main_thread(*this, 0, 0x0)
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
thread<W>* multithreading<W>::get_thread(int64_t tid)
{
	auto it = threads.find(tid);
	if (it == threads.end()) return nullptr;
	return it->second;
}

template <int W>
void multithreading<W>::suspend_and_yield()
{
    // don't go through the ardous yielding process when alone
    if (suspended.empty()) return;
    // suspend current thread
    auto* thread = get_thread();
    thread->suspend();
    // resume some other thread
    thread->yield();
}

template <int W>
void multithreading<W>::erase_thread(int64_t tid)
{
	auto it = threads.find(tid);
	assert(it != threads.end());
	threads.erase(it);
}
template <int W>
void multithreading<W>::erase_suspension(thread_t* t)
{
  for (auto it = suspended.begin(); it != suspended.end();)
  {
      if (*it == t) {
          it = suspended.erase(it);
      }
      else {
          ++it;
      }
  }
}

template <int W>
void setup_multithreading(Machine<W>& machine)
{
	auto* mt = new multithreading<W>(machine);
	// exit & exit_group
	machine.install_syscall_handler(93,
	[mt] (Machine<W>& machine) {
		const uint32_t status = machine.template sysarg<uint32_t> (0);
		const int64_t tid = mt->get_thread()->tid;
		THPRINT(">>> Exit on tid=%ld, exit code = %u\n",
				tid, status);
		if (tid != 0) {
			// exit thread instead
			mt->get_thread()->exit();
			// should be a new thread now
			assert(mt->get_thread()->tid != tid);
			return machine.cpu.reg(RISCV::REG_ARG0);
		}
		machine.stop();
		return status;
	});
	// exit_group
	machine.install_syscall_handler(94, machine.get_syscall_handler(93));
	// set_tid_address
	machine.install_syscall_handler(96,
	[mt] (Machine<W>& machine) {
		// TODO: set clear ctid
		return mt->get_thread()->tid;
	});
	// set_robust_list
	machine.install_syscall_handler(99,
	[] (Machine<W>& machine) {
		return 0;
	});
	// sched_yield
	machine.install_syscall_handler(124,
	[mt] (Machine<W>& machine) {
		THPRINT(">>> sched_yield()\n");
		// begone!
		mt->suspend_and_yield();
		// preserve A0
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
	[mt] (Machine<W>& machine) {
		THPRINT(">>> gettid() = %ld\n", mt->get_thread()->tid);
		return mt->get_thread()->tid;
	});
	// futex
	machine.install_syscall_handler(98,
	[mt] (Machine<W>& machine) {
		#define FUTEX_WAIT 0
		const uint32_t addr = machine.template sysarg<uint32_t> (0);
		const int  futex_op = machine.template sysarg<int> (1);
		const int       val = machine.template sysarg<int> (2);
		THPRINT(">>> futex(0x%X, op=%d, val=%d)\n", addr, futex_op, val);
		if ((futex_op & 0xF) == FUTEX_WAIT)
	    {
			THPRINT("FUTEX: Waiting for unlock... uaddr=0x%X val=%d\n", addr, val);
			while (machine.memory.template read<uint32_t> (addr) == val) {
				mt->suspend_and_yield();
			}
	    }
	    return 0;
	});
	// clone
	machine.install_syscall_handler(220,
	[mt] (Machine<W>& machine) {
		/* int clone(int (*fn)(void *arg), void *child_stack, int flags, void *arg,
		             void *parent_tidptr, void *tls, void *child_tidptr) */
		const uint32_t  func = machine.template sysarg<uint32_t> (0);
		const uint32_t stack = machine.template sysarg<uint32_t> (1);
		const int      flags = machine.template sysarg<int> (2);
		const uint32_t  args = machine.template sysarg<uint32_t> (3);
		const uint32_t  ptid = machine.template sysarg<uint32_t> (4);
		const uint32_t   tls = machine.template sysarg<uint32_t> (5);
		const uint32_t  ctid = machine.template sysarg<uint32_t> (6);
		auto* parent = mt->get_thread();
		THPRINT(">>> clone(func=0x%X, stack=0x%X, flags=%x, parent=%p)\n",
				func, stack, flags, parent);
		auto* thread = mt->create(parent, flags, ctid, stack, tls);
		parent->suspend();
		// store return value for parent: child TID
		parent->stored_regs.get(RISCV::REG_ARG0) = thread->tid;
		// activate and return 0 for the child
		thread->activate(func, args);
		return 0;
	});
}

template
void setup_multithreading<4>(Machine<4>& machine);
