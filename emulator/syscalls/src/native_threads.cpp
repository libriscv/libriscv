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

#ifdef RISCV_PAGE_TRAPS_ENABLED
	// super fast threads
	if (arena != nullptr)
	{
		struct Data {
			Machine<W>& machine;
			multithreading<W>* mt;
			sas_alloc::Arena* arena;
		};
		auto* data = new Data { machine, mt, arena };
		machine.add_destructor_callback([data] { delete data; });

		// custom jump trap for clone threadcall
		auto& clone_page = machine.memory.install_shared_page(0xFFFFE,
			Page{{ .read = false, .write = false, .exec = false }, nullptr});
		// create an execution trap on the page
		clone_page.set_trap(
		[data] (riscv::Page&, uint32_t, int, int64_t) -> int64_t {
			auto& machine = data->machine;
			auto* mt    = data->mt;
			auto* arena = data->arena;
			// invoke clone threadcall
			const uint32_t   tls = arena->malloc(STACK_SIZE);
			if (UNLIKELY(tls == 0)) {
				fprintf(stderr,
					"Error: Thread stack allocation failed: %#x\n", tls);
				machine.cpu.reg(RISCV::REG_ARG0) = -1;
				// return back to caller
				machine.cpu.jump(machine.cpu.reg(riscv::RISCV::REG_RA));
				return 0;
			}
			const uint32_t stack = ((tls + STACK_SIZE) & ~0xF);
			const uint32_t  func = machine.template sysarg<uint32_t> (0);
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
			machine.cpu.reg(riscv::RISCV::REG_RA) = 0xFFFFD000;
			// geronimo!
			machine.cpu.jump(func);
			// first argument is Thread* or Thread&
			machine.cpu.reg(riscv::RISCV::REG_ARG0) = tls;
			return 0;
		});
		// custom jump trap for exit threadcall
		auto& exit_page = machine.memory.install_shared_page(0xFFFFD,
			Page{{ .read = false, .write = false, .exec = false }, nullptr});
		// create an execution trap on the page
		exit_page.set_trap(
		[data] (riscv::Page&, uint32_t, int, int64_t) -> int64_t {
			auto& machine = data->machine;
			auto* mt    = data->mt;
			auto* arena = data->arena;

			auto self = machine.cpu.reg(riscv::RISCV::REG_TP);
			// TODO: check this return value
			arena->free(self);
			// exit thread instead
			mt->get_thread()->exit();
			// we need to jump ahead because pre-instruction
			machine.cpu.registers().pc += 4;
			return 0;
		});
	} // arena provided
#endif
	return mt;
}

template
multithreading<4>* setup_native_threads<4>(Machine<4>&, sas_alloc::Arena*);
template
multithreading<8>* setup_native_threads<8>(Machine<8>&, sas_alloc::Arena*);
