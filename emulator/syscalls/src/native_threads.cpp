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
		const uint32_t stack = (machine.template sysarg<uint32_t> (0) & ~0xF);
		const uint32_t  func = machine.template sysarg<uint32_t> (1);
		const uint32_t   tls = machine.template sysarg<uint32_t> (2);
		auto* thread = mt->create(
			CHILD_SETTID, tls, 0x0, stack, tls);
		// suspend and store return value for parent: child TID
		auto* parent = mt->get_thread();
		parent->suspend(thread->tid);
		// activate and setup a function call
		thread->activate();
		// the cast is a work-around for a compiler bug
		machine.setup_call(func, (const uint32_t) tls);
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
	[mt] (Machine<W>& machine) {
		// begone!
		if (mt->block(machine.template sysarg<int> (0)))
			// preserve A0 for the new thread
			return machine.cpu.reg(RISCV::REG_ARG0);
		// error, we didn't block
		return -1u;
	});
	// unblock (w/reason)
	machine.install_syscall_handler(THREADS_SYSCALL_BASE+5,
	[mt] (Machine<W>& machine) {
		if (!mt->wakeup_blocked(machine.template sysarg<int> (0)))
			return -1u;
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

		auto& page = machine.memory.create_page(0xFFFFE);
		page.attr.write = false;
		page.attr.read = false;
		page.attr.exec = true;
		// create an execution trap on the page
		page.set_trap(
		[data] (riscv::Page&, uint32_t sysn, int, int64_t) -> int64_t {
			auto& machine = data->machine;
			auto* mt    = data->mt;
			auto* arena = data->arena;
			// invoke a system call
			//printf("Threadcall: %#x %d\n", sysn, sysn / 4);
			switch (sysn / 4) {
				case 64: {
					// ultra-fast clone
					const uint32_t   tls = arena->malloc(STACK_SIZE);
					if (UNLIKELY(tls == 0)) {
						fprintf(stderr,
							"Error: Thread stack allocation failed: %#x\n", tls);
						machine.cpu.reg(RISCV::REG_ARG0) = -1;
						break;
					}
					const uint32_t stack = ((tls + STACK_SIZE) & ~0xF);
					const uint32_t  func = machine.template sysarg<uint32_t> (0);
					const uint32_t  data = machine.template sysarg<uint32_t> (1);
					const uint32_t exitf = machine.template sysarg<uint32_t> (2);
					machine.memory.template write<uint32_t> (tls + 4, func);
					machine.memory.template write<uint32_t> (tls + 8, data);
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
					// the cast is a work-around for a compiler bug
					machine.cpu.reg(riscv::RISCV::REG_ARG0) = (uint32_t) tls;
					machine.cpu.reg(riscv::RISCV::REG_RA) = exitf;
					machine.cpu.jump(func);
					return 0;
				}
				default:
					printf("Error: Unknown threadcall: %d\n", sysn / 4);
					machine.cpu.reg(RISCV::REG_ARG0) = -1;
			}
			// return to caller
			const auto retaddr = machine.cpu.reg(riscv::RISCV::REG_RA);
			machine.cpu.jump(retaddr);
			return 0;
		});
	} // arena provided
	return mt;
}

template
multithreading<4>* setup_native_threads<4>(Machine<4>&, sas_alloc::Arena*);
