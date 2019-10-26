#pragma once

template <int W>
CPU<W>::CPU(Machine<W>& machine)
	: m_machine { machine }
{
}

template <int W>
inline void CPU<W>::breakpoint(address_t addr, breakpoint_t func) {
	this->m_breakpoints[addr] = func;
}

template <int W>
inline void CPU<W>::default_pausepoint(CPU& cpu)
{
	CPU<W>::print_and_pause(cpu);
}
