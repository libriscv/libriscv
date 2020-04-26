#include "threads.cpp"

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
		auto* thread = mt->create(flags, ctid, ptid, stack, tls);
		parent->suspend();
		// store return value for parent: child TID
		parent->stored_regs.get(RISCV::REG_ARG0) = thread->tid;
		// activate and return 0 for the child
		thread->activate();
		return 0;
	});
}

template
void setup_multithreading<4>(State<4>&, Machine<4>& machine);
