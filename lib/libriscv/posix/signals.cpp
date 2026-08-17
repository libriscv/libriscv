#include "signals.hpp"

#include "../machine.hpp"
#include "../internal_common.hpp"
#include "../threads.hpp"

namespace riscv {

/// @brief The layout of the Linux rt_sigframe on RISC-V.
/// @details The kernel pushes { siginfo_t info; struct ucontext uc; } on the
/// signal stack and passes &info and &uc as the second and third argument of
/// an SA_SIGINFO handler. Runtimes that inspect the siginfo (eg. the Go
/// runtime reads si_code before anything else) fault immediately without it.
template <int W>
struct SignalFrameLayout {
	using address_t = address_type<W>;
	static constexpr size_t siginfo_size = 128;
	/// siginfo_t: si_signo, si_errno, si_code, then a union that begins on
	/// the next natural alignment boundary. si_pid and si_uid come first.
	static constexpr size_t si_union    = (W == 8) ? 16 : 12;
	/// ucontext: uc_flags, uc_link, uc_stack (ss_sp, ss_flags, ss_size),
	/// then 1024 bits reserved for uc_sigmask, then the machine context.
	static constexpr size_t uc_stack    = 2 * W;
	static constexpr size_t uc_sigmask  = 5 * W;
	static constexpr size_t mcontext    = (uc_sigmask + 128 + 7) & ~size_t(7);
	/// mcontext: 32 general registers where the first one is the PC,
	/// followed by the fp state (widest union member is 528 bytes).
	static constexpr size_t fpstate     = mcontext + 32 * W;
	static constexpr size_t ucontext_size = (fpstate + 528 + 15) & ~size_t(15);
	static constexpr size_t frame_size  = siginfo_size + ucontext_size;

	std::array<uint8_t, frame_size> data {};

	void put_word(size_t offset, address_t value) {
		std::memcpy(&data[offset], &value, sizeof(value));
	}
	void put_u32(size_t offset, uint32_t value) {
		std::memcpy(&data[offset], &value, sizeof(value));
	}
};

template <int W>
Signals<W>::Signals() {}
template <int W>
Signals<W>::~Signals() {}

template <int W>
SignalAction<W>& Signals<W>::get(int sig) {
	if (sig > 0)
		return signals.at(sig-1);
	throw MachineException(ILLEGAL_OPERATION, "Signal 0 invoked");
}

template <int W>
void Signals<W>::enter(Machine<W>& machine, int sig)
{
	if (sig == 0) return;

	using address_t = address_type<W>;
	auto& sigact = this->get(sig);
	auto* thread = machine.threads().get_thread();
	auto& pt = per_thread(thread->tid);

	// Remember the interrupted state for a future sigreturn. A signal lands on
	// any instruction, vector loops included, and the handler will run vector
	// code of its own, so the vector state has to be part of what is saved.
	pt.sigret.regs.copy_from(
		machine.register_copy_options(), machine.cpu.registers());

	address_t sp = machine.cpu.reg(REG_SP);
	if (sigact.altstack) {
		// Change to alternate per-thread stack
		sp = pt.stack.ss_sp + pt.stack.ss_size;
	}

	using Layout = SignalFrameLayout<W>;
	Layout frame;
	// The signal frame is 16-byte aligned, like any other RISC-V stack frame
	sp = (sp - Layout::frame_size) & ~address_t(0xF);
	const address_t info_addr = sp;
	const address_t uctx_addr = sp + Layout::siginfo_size;

	// siginfo_t: the signal was sent by a thread of this same process
	static constexpr int32_t si_tkill = -6;
	frame.put_u32(0, uint32_t(sig));           // si_signo
	frame.put_u32(4, 0);                       // si_errno
	frame.put_u32(8, uint32_t(si_tkill));      // si_code
	frame.put_u32(Layout::si_union + 0, MAIN_THREAD_TID); // si_pid
	frame.put_u32(Layout::si_union + 4, 0);    // si_uid

	// ucontext: no flags, no linked context, the stack we are running on.
	// Every field below is relative to the start of the ucontext.
	const size_t uc = Layout::siginfo_size;
	frame.put_word(uc + Layout::uc_stack + 0, pt.stack.ss_sp);
	frame.put_u32(uc + Layout::uc_stack + W, uint32_t(pt.stack.ss_flags));
	frame.put_word(uc + Layout::uc_stack + 2 * W, pt.stack.ss_size);
	frame.put_word(uc + Layout::uc_sigmask, sigact.mask);

	// mcontext: the PC we would have resumed at, then x1 through x31.
	// NOTE: The fp state is left zeroed, and sigreturn is not implemented,
	// so handlers cannot resume with a modified context. They return the
	// ordinary way instead, to the return address of the interrupted call.
	const auto& regs = machine.cpu.registers();
	frame.put_word(uc + Layout::mcontext, machine.cpu.pc() + 4);
	for (unsigned i = 1; i < 32; i++) {
		frame.put_word(uc + Layout::mcontext + i * W, regs.get(i));
	}

	machine.copy_to_guest(sp, frame.data.data(), frame.data.size());

	machine.cpu.reg(REG_SP)   = sp;
	machine.cpu.reg(REG_ARG0) = sig;
	machine.cpu.reg(REG_ARG1) = info_addr;
	machine.cpu.reg(REG_ARG2) = uctx_addr;
	// Like Linux, return into a trampoline that invokes rt_sigreturn, which
	// is the only way back to the interrupted code once we changed the stack.
	if (machine.memory.sigreturn_address() != 0x0)
		machine.cpu.reg(REG_RA) = machine.memory.sigreturn_address();

	THPRINT(machine,
		"<<< signal %d handler 0x%lX altstack=%d sp=0x%lX info=0x%lX uctx=0x%lX\n",
		sig, (long)sigact.handler, sigact.altstack,
		(long)sp, (long)info_addr, (long)uctx_addr);

	// We have to jump to handler-4 because we are mid-instruction
	// WARNING: Assumption.
	machine.cpu.jump(sigact.handler - 4);
}

template <int W>
void Signals<W>::leave(Machine<W>& machine)
{
	using address_t = address_type<W>;
	using Layout = SignalFrameLayout<W>;
	auto* thread = machine.threads().get_thread();
	auto& pt = per_thread(thread->tid);

	// Linux restores from the ucontext the handler was given, which the
	// handler is allowed to have modified. The frame sits right above the
	// stack pointer the handler was entered with.
	const address_t uctx_addr = machine.cpu.reg(REG_SP) + Layout::siginfo_size;
	std::array<uint8_t, 32 * sizeof(address_t)> gregs;
	address_t pc = 0;
	try {
		machine.copy_from_guest(gregs.data(),
			uctx_addr + Layout::mcontext, gregs.size());
		std::memcpy(&pc, &gregs[0], sizeof(pc));
	} catch (...) {
		// A handler that lost its stack cannot be resumed from the ucontext,
		// so fall back to the state we saved when entering the handler.
		machine.cpu.registers().copy_from(
			machine.register_copy_options(), pt.sigret.regs);
		THPRINT(machine, "<<< sigreturn: unreadable ucontext, restored saved state\n");
		return;
	}

	// Our signal frame only carries the integer registers, where Linux would
	// also carry the FP and vector extension state. The state we saved on the
	// way in stands in for that part of the frame, so that a handler running
	// vector or FP code does not clobber the interrupted thread. The integer
	// registers then come from the ucontext, which the handler may have edited.
	machine.cpu.registers().copy_from(
		machine.register_copy_options(), pt.sigret.regs);

	auto& regs = machine.cpu.registers();
	for (unsigned i = 1; i < 32; i++) {
		address_t value = 0;
		std::memcpy(&value, &gregs[i * sizeof(address_t)], sizeof(value));
		regs.get(i) = value;
	}

	THPRINT(machine, "<<< sigreturn to 0x%lX (sp=0x%lX)\n",
		(long)pc, (long)machine.cpu.reg(REG_SP));

	// We are mid-instruction: the dispatcher adds the instruction length
	machine.cpu.jump(pc - 4);
}

	INSTANTIATE_32_IF_ENABLED(Signals);
	INSTANTIATE_64_IF_ENABLED(Signals);
	INSTANTIATE_128_IF_ENABLED(Signals);
} // riscv
