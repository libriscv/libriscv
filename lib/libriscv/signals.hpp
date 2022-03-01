#pragma once

namespace riscv {

template <int W>
struct SignalStack {
	address_type<W> ss_sp = 0x0;
	int             ss_flags = 0x0;
	size_t          ss_size = 0;
};

template <int W>
struct SignalAction {
	address_type<W> handler = ~(address_type<W>)0x0;
	bool altstack = false;
};

template <int W>
struct Signals {
	std::array<SignalAction<W>, 64> sig {};
	SignalStack<W> stack {};
};

}
