#include "threads.hpp"
#include <cassert>
#include <cstdio>
#include <sched.h>
using namespace riscv;

#include "threads.cpp"

template <int W>
void setup_native_threads(State<W>& state, Machine<W>& machine)
{
	auto* mt = new multithreading<W>(machine);
	machine.add_destructor_callback([mt] { delete mt; });

	// exit
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
	// sched_yield
	machine.install_syscall_handler(124,
	[mt] (Machine<W>& machine) {
		THPRINT(">>> sched_yield()\n");
		// begone!
		mt->suspend_and_yield();
		// preserve A0 for the new thread
		return machine.cpu.reg(RISCV::REG_ARG0);
	});
	// 500: microclone
	machine.install_syscall_handler(500,
	[mt] (Machine<W>& machine) {
		const uint32_t stack = machine.template sysarg<uint32_t> (0);
		const uint32_t  func = machine.template sysarg<uint32_t> (1);
		const uint32_t   tls = machine.template sysarg<uint32_t> (2);
		const uint32_t  ctid = machine.template sysarg<uint32_t> (3);
		auto* parent = mt->get_thread();
		auto* thread = mt->create(parent,
			CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID, ctid, 0x0, stack, tls);
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
	// yield_to
	machine.install_syscall_handler(501,
	[mt] (Machine<W>& machine) {
		mt->yield_to(machine.template sysarg<uint32_t> (0));
		// preserve A0 for the new thread
		return machine.cpu.reg(RISCV::REG_ARG0);
	});
}

template
void setup_native_threads<4>(State<4>&, Machine<4>& machine);
