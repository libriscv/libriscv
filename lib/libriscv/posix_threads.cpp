#include "threads.hpp"

namespace riscv {

template <int W>
void Machine<W>::setup_posix_threads()
{
	this->m_mt.reset(new MultiThreading<W>(*this));

	// exit & exit_group
	this->install_syscall_handler(93,
	[] (Machine<W>& machine) {
		const uint32_t status = machine.template sysarg<uint32_t> (0);
		THPRINT(">>> Exit on tid=%d, exit code = %d\n",
				machine.threads().get_tid(), (int) status);
		// Exit returns true if the program ended
		if (!machine.threads().get_thread()->exit()) {
			return;
		}
		machine.stop();
		machine.set_result(status);
	});
	// exit_group
	this->install_syscall_handler(94, syscall_handlers.at(93));
	// set_tid_address
	this->install_syscall_handler(96,
	[] (Machine<W>& machine) {
		const int clear_tid = machine.template sysarg<address_type<W>> (0);
		THPRINT(">>> set_tid_address(0x%X)\n", clear_tid);

		machine.threads().get_thread()->clear_tid = clear_tid;
		machine.set_result(machine.threads().get_tid());
	});
	// set_robust_list
	this->install_syscall_handler(99,
	[] (Machine<W>& machine) {
		machine.set_result(0);
	});
	// sched_yield
	this->install_syscall_handler(124,
	[] (Machine<W>& machine) {
		THPRINT(">>> sched_yield()\n");
		// begone!
		machine.threads().suspend_and_yield();
	});
	// tgkill
	this->install_syscall_handler(131,
	[] (Machine<W>& machine) {
		const int tid = machine.template sysarg<int> (1);
		THPRINT(">>> tgkill on tid=%d\n", tid);
		auto* thread = machine.threads().get_thread(tid);
		if (thread != nullptr) {
			// exit thread instead
			thread->exit();
			return;
		}
		machine.stop();
	});
	// gettid
	this->install_syscall_handler(178,
	[] (Machine<W>& machine) {
		THPRINT(">>> gettid() = %ld\n", machine.threads().get_tid());
		machine.set_result(machine.threads().get_tid());
	});
	// futex
	this->install_syscall_handler(98,
	[] (Machine<W>& machine) {
		#define FUTEX_WAIT 0
		#define FUTEX_WAKE 1
		const auto addr = machine.template sysarg<address_type<W>> (0);
		const int futex_op = machine.template sysarg<int> (1);
		const int      val = machine.template sysarg<int> (2);
		THPRINT(">>> futex(0x%lX, op=%d, val=%d)\n", (long) addr, futex_op, val);
		if ((futex_op & 0xF) == FUTEX_WAIT)
	    {
			THPRINT("FUTEX: Waiting for unlock... uaddr=0x%lX val=%d\n", (long) addr, val);
			while (machine.memory.template read<address_type<W>> (addr) == val) {
				if (machine.threads().suspend_and_yield()) {
					return;
				}
				machine.cpu.trigger_exception(DEADLOCK_REACHED);
			}
			machine.set_result(0);
			return;
		} else if ((futex_op & 0xF) == FUTEX_WAKE) {
			THPRINT("FUTEX: Waking others on %d\n", val);
			if (machine.threads().suspend_and_yield()) {
				return;
			}
			machine.set_result(0);
			return;
		}
		machine.set_result(-EINVAL);
	});
	// clone
	this->install_syscall_handler(220,
	[] (Machine<W>& machine) {
		/* int clone(int (*fn)(void *arg), void *child_stack, int flags, void *arg,
		             void *parent_tidptr, void *tls, void *child_tidptr) */
		const int  flags = machine.template sysarg<int> (0);
		const auto stack = machine.template sysarg<address_type<W>> (1);
#ifdef THREADS_DEBUG
		const auto  func = machine.template sysarg<address_type<W>> (2);
		const auto  args = machine.template sysarg<address_type<W>> (3);
#endif
		const auto  ptid = machine.template sysarg<address_type<W>> (4);
		const auto   tls = machine.template sysarg<address_type<W>> (5);
		const auto  ctid = machine.template sysarg<address_type<W>> (6);
		auto* parent = machine.threads().get_thread();
		THPRINT(">>> clone(func=0x%X, stack=0x%X, flags=%x, args=0x%X,"
				" parent=%p, ctid=0x%X ptid=0x%X, tls=0x%X)\n",
				func, stack, flags, args, parent, ctid, ptid, tls);
		auto* thread = machine.threads().create(flags, ctid, ptid, stack, tls);
		// store return value for parent: child TID
		parent->suspend(thread->tid);
		// activate and return 0 for the child
		thread->activate();
		machine.set_result(0);
	});
	// prlimit64
	this->install_syscall_handler(261,
	[] (Machine<W>& machine) {
		const int resource = machine.template sysarg<int> (1);
		const auto old_addr = machine.template sysarg<address_type<W>> (3);
		struct {
			address_type<W> cur = 0;
			address_type<W> max = 0;
		} lim;
		constexpr int RISCV_RLIMIT_STACK = 3;
		if (old_addr != 0) {
			if (resource == RISCV_RLIMIT_STACK) {
				lim.cur = 0x200000;
				lim.max = 0x200000;
			}
			machine.copy_to_guest(old_addr, &lim, sizeof(lim));
			machine.set_result(0);
		} else {
			machine.set_result(-EINVAL);
		}
	});
}

template void Machine<4>::setup_posix_threads();
template void Machine<8>::setup_posix_threads();
} // riscv
