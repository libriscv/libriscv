#include "machine.hpp"

template <int W>
inline Machine<W>::Machine(std::vector<uint8_t> binary, bool verbose)
	: verbose_machine{verbose}, cpu(*this), memory(*this, std::move(binary))
{
	cpu.reset();
}

template <int W>
inline void Machine<W>::stop() noexcept {
	m_stopped = true;
}
template <int W>
inline bool Machine<W>::stopped() const noexcept {
	return m_stopped;
}

template <int W>
inline void Machine<W>::simulate()
{
	cpu.simulate();
}

template <int W>
inline void Machine<W>::install_syscall_handler(int sysn, syscall_t handler)
{
	m_syscall_handlers[sysn] = handler;
}

template <int W>
inline void Machine<W>::system_call(int syscall_number)
{
	auto it = m_syscall_handlers.find(syscall_number);
	if (it != m_syscall_handlers.end()) {
		address_t ret = it->second(*this);
		cpu.reg(RISCV::REG_RETVAL) = ret;
		return;
	}
	//cpu.trigger_interrupt(UNIMPLEMENTED_SYSCALL);
	cpu.reg(RISCV::REG_RETVAL) = -1;
}
