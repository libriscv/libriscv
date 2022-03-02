#include "machine.hpp"
#include "signals.hpp"
#include "threads.hpp"


namespace riscv {

template <int W>
Signals<W>::Signals() {}
template <int W>
Signals<W>::~Signals() {}

template <int W>
void Signals<W>::enter(Machine<W>& machine, int sig)
{
	auto& sigact = signals.at(sig);
	if (sigact.altstack) {
		auto* thread = machine.threads().get_thread();
		// Change to alternate per-thread stack
		auto& stack = per_thread(thread->tid).stack;
		machine.cpu.reg(REG_SP) = stack.ss_sp + stack.ss_size;
		machine.cpu.reg(REG_SP) &= ~(address_type<W>)0xF;
	}
	// We have to jump to handler-4 because we are mid-instruction
	// WARNING: Assumption.
	machine.cpu.jump(sigact.handler - 4);
}

	template struct Signals<4>;
	template struct Signals<8>;
	template struct Signals<16>;
} // riscv
