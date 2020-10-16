#include <include/threads.hpp>
#include <include/native_heap.hpp>
#include <cassert>
#include <cstdio>
using namespace riscv;
#ifndef THREADS_SYSCALL_BASE
static const int THREADS_SYSCALL_BASE = 500;
#endif
static const uint32_t STACK_SIZE = 256 * 1024;
#include "threads.cpp"

template <int W>
multithreading<W>* setup_native_threads(
	Machine<W>& machine, sas_alloc::Arena* arena)
{
	auto* mt = new multithreading<W>(machine);
	machine.add_destructor_callback([mt] { delete mt; });

	// 500: microclone
	machine.install_syscall_handler(THREADS_SYSCALL_BASE+0,
	[mt] (Machine<W>& machine) {
		const auto stack = (machine.template sysarg<address_type<W>> (0) & ~0xF);
		const auto  func = machine.template sysarg<address_type<W>> (1);
		const auto   tls = machine.template sysarg<address_type<W>> (2);
		const auto flags = machine.template sysarg<uint32_t> (3);
		THPRINT(">>> clone(func=0x%X, stack=0x%X, tls=0x%X)\n",
				func, stack, tls);
		auto* thread = mt->create(
			CHILD_SETTID | flags, tls, 0x0, stack, tls);
		// suspend and store return value for parent: child TID
		auto* parent = mt->get_thread();
		parent->suspend(thread->tid);
		// activate and setup a function call
		thread->activate();
		// the cast is a work-around for a compiler bug
		// NOTE: have to start at DST-4 here!!!
		machine.setup_call(func-4, (const address_type<W>) tls);
		// preserve A0 for the new child thread
		return machine.cpu.reg(RISCV::REG_ARG0);
	});
	// exit
	machine.install_syscall_handler(THREADS_SYSCALL_BASE+1,
	[mt] (Machine<W>& machine) {
		const int status = machine.template sysarg<int> (0);
		const int tid = mt->get_thread()->tid;
		THPRINT(">>> Exit on tid=%d, exit status = %d\n",
				tid, (int) status);
		if (tid != 0) {
			// exit thread instead
			mt->get_thread()->exit();
			// should be a new thread now
			assert(mt->get_thread()->tid != tid);
			return machine.cpu.reg(RISCV::REG_ARG0);
		}
		machine.stop();
		return (address_type<W>) status;
	});
	// sched_yield
	machine.install_syscall_handler(THREADS_SYSCALL_BASE+2,
	[mt] (Machine<W>& machine) {
		// begone!
		mt->suspend_and_yield();
		// preserve A0 for the new thread
		return machine.cpu.reg(RISCV::REG_ARG0);
	});
	// yield_to
	machine.install_syscall_handler(THREADS_SYSCALL_BASE+3,
	[mt] (Machine<W>& machine) {
		mt->yield_to(machine.template sysarg<uint32_t> (0));
		// preserve A0 for the new thread
		return machine.cpu.reg(RISCV::REG_ARG0);
	});
	// block (w/reason)
	machine.install_syscall_handler(THREADS_SYSCALL_BASE+4,
	[mt] (Machine<W>& machine) -> long {
		// begone!
		if (mt->block(machine.template sysarg<int> (0)))
			// preserve A0 for the new thread
			return machine.cpu.reg(RISCV::REG_ARG0);
		// error, we didn't block
		return -1;
	});
	// unblock (w/reason)
	machine.install_syscall_handler(THREADS_SYSCALL_BASE+5,
	[mt] (Machine<W>& machine) -> long {
		if (!mt->wakeup_blocked(machine.template sysarg<int> (0)))
			return -1;
		// preserve A0 for the new thread
		return machine.cpu.reg(RISCV::REG_ARG0);
	});
	// unblock thread
	machine.install_syscall_handler(THREADS_SYSCALL_BASE+6,
	[mt] (Machine<W>& machine) {
		mt->unblock(machine.template sysarg<int> (0));
		// preserve A0 for the new thread
		return machine.cpu.reg(RISCV::REG_ARG0);
	});

	// super fast "direct" threads
	if (arena != nullptr)
	{
		struct Data {
			multithreading<W>* mt;
			sas_alloc::Arena* arena;
		};
		auto* data = new Data { mt, arena };
		machine.add_destructor_callback([data] { delete data; });

		// N+8: clone threadcall
		machine.install_syscall_handler(THREADS_SYSCALL_BASE+8,
		[data] (Machine<W>& machine) -> long {
			auto* mt    = data->mt;
			auto* arena = data->arena;
			// invoke clone threadcall
			const auto tls = arena->malloc(STACK_SIZE);
			if (UNLIKELY(tls == 0)) {
				fprintf(stderr,
					"Error: Thread stack allocation failed: %#x\n", tls);
				return -1;
			}
			const auto stack = ((tls + STACK_SIZE) & ~0xF);
			const auto  func = machine.template sysarg<address_type<W>> (0);
			const auto  fini = machine.template sysarg<address_type<W>> (1);
			auto* thread = mt->create(
				CHILD_SETTID, tls, 0x0, stack, tls);
			// set PC back to clone point - 4
			machine.cpu.registers().pc =
				machine.cpu.reg(riscv::RISCV::REG_RA) - 4;
			// suspend and store return value for parent: child TID
			auto* parent = mt->get_thread();
			parent->suspend(thread->tid);
			// activate and setup a function call
			thread->activate();
			// exit into the exit function which frees the thread
			machine.cpu.reg(riscv::RISCV::REG_RA) = fini;
			// move 6 arguments back
			std::memmove(&machine.cpu.reg(10), &machine.cpu.reg(12),
				6 * sizeof(address_type<W>));
			// geronimo!
			machine.cpu.jump(func - 4);
			return machine.cpu.reg(riscv::RISCV::REG_RETVAL);
		});
		// N+9: exit threadcall
		machine.install_syscall_handler(THREADS_SYSCALL_BASE+9,
		[data] (Machine<W>& machine) {
			auto* mt    = data->mt;
			auto* arena = data->arena;

			auto retval = machine.cpu.reg(riscv::RISCV::REG_RETVAL);
			auto self = machine.cpu.reg(riscv::RISCV::REG_TP);
			// TODO: check this return value
			arena->free(self);
			// exit thread instead
			mt->get_thread()->exit();
			// return value from exited thread
			return retval;
		});
	} // arena provided
	return mt;
}

template
multithreading<4>* setup_native_threads<4>(Machine<4>&, sas_alloc::Arena*);
template
multithreading<8>* setup_native_threads<8>(Machine<8>&, sas_alloc::Arena*);
