#pragma once
#include <set>

namespace riscv {
template <int W> struct Registers;

template <int W>
struct SignalStack {
	address_type<W> ss_sp = 0x0;
	int             ss_flags = 0x0;
	address_type<W> ss_size = 0;
};

template <int W>
struct SignalAction {
	static constexpr address_type<W> SIG_UNSET = ~(address_type<W>)0x0;
	bool is_unset() const noexcept {
		return handler == SIG_UNSET;
	}
	address_type<W> handler = SIG_UNSET;
	bool altstack = false;
};

template <int W>
struct SignalReturn {
	Registers<W> regs;
};

template <int W>
struct SignalPerThread {
	SignalStack<W> stack;
	SignalReturn<W> sigret;
};

template <int W>
struct Signals {
	void enter(Machine<W>&, int sig);

	std::array<SignalAction<W>, 64> signals {};

	// TODO: Lock this in the future, for multiproessing
	auto& per_thread(int tid) { return m_per_thread[tid]; }

	Signals();
	~Signals();
private:
	std::map<int, SignalPerThread<W>> m_per_thread;
};

}
