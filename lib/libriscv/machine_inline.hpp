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
inline void Machine<W>::reset()
{
	cpu.reset();
	memory.reset();
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
		// EBREAK should not modify registers
		if (syscall_number != 0) {
			cpu.reg(RISCV::REG_RETVAL) = ret;
		}
		return;
	}
	if (UNLIKELY(verbose_machine)) {
		fprintf(stderr, ">>> Warning: Unhandled syscall %d\n", syscall_number);
	}
	// EBREAK should not modify registers
	if (syscall_number != 0) {
		cpu.reg(RISCV::REG_RETVAL) = -1;
	}
}

template <int W>
template <typename T>
inline T Machine<W>::sysarg(int idx) const
{
	return static_cast<T> (cpu.reg(RISCV::REG_ARG0 + idx));
}

#ifdef RISCV_DEBUG

template <int W>
inline void Machine<W>::break_now()
{
	cpu.break_now();
}

#endif
