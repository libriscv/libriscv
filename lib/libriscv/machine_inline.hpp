
template <int W>
inline Machine<W>::Machine(std::vector<uint8_t> binary, bool protect)
	: cpu(*this), memory(*this, std::move(binary), protect)
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
			if (UNLIKELY(this->verbose_jumps)) {
				printf("SYSCALL %d returned %ld (0x%lX)\n", syscall_number,
						(long) ret, (long) ret);
			}
		}
		return;
	}
	if (throw_on_unhandled_syscall == false)
	{
		if (UNLIKELY(verbose_machine)) {
			fprintf(stderr, ">>> Warning: Unhandled syscall %d\n", syscall_number);
		}
		// EBREAK should not modify registers
		if (syscall_number != 0) {
			cpu.reg(RISCV::REG_RETVAL) = -ENOSYS;
		}
	}
	else {
		throw MachineException("Unhandled system call: " + std::to_string(syscall_number));
	}
}

template <int W>
template <typename T>
inline T Machine<W>::sysarg(int idx) const
{
	return static_cast<T> (cpu.reg(RISCV::REG_ARG0 + idx));
}

template <int W>
address_type<W> Machine<W>::copy_to_guest(address_t dst, const void* buf, size_t len)
{
	memory.memcpy(dst, buf, len);
	return dst + len;
}

#ifdef RISCV_DEBUG

template <int W>
inline void Machine<W>::break_now()
{
	cpu.break_now();
}

#endif
