
template <int W>
inline Machine<W>::Machine(const std::vector<uint8_t>& binary, bool protect)
	: cpu(*this), memory(*this, binary, protect)
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
inline void Machine<W>::simulate(uint64_t max_instr)
{
	this->m_stopped = false;
	if (max_instr != 0) {
		max_instr += cpu.registers().counter;
		while (!this->stopped()) {
			cpu.simulate();
			if (UNLIKELY(cpu.registers().counter >= max_instr)) break;
		}
	}
	else {
		while (!this->stopped()) {
			cpu.simulate();
		}
	}
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

template <int W>
inline long Machine<W>::vmcall(const std::string& function_name,
								address_t a0, address_t a1, address_t a2)
{
	this->setup_call(function_name, "_exit", a0, a1, a2);
	long retval = -1;
	this->install_syscall_handler(93,
		[&retval] (auto& machine) -> long {
			retval = machine.template sysarg<long> (0);
			machine.stop();
			return 0; // have to return, even when simulation stops
		});
	this->simulate();
	return retval;
}


template <int W>
inline void Machine<W>::setup_call(
		const std::string& call_sym,
		const std::string& return_sym,
		address_t arg0, address_t arg1, address_t arg2)
{
	address_t call_addr = memory.resolve_address(call_sym);
	address_t retn_addr = memory.resolve_address(return_sym);
	cpu.reg(RISCV::REG_RA) = retn_addr;
	cpu.reg(RISCV::REG_ARG0) = arg0;
	cpu.reg(RISCV::REG_ARG1) = arg1;
	cpu.reg(RISCV::REG_ARG2) = arg2;
	cpu.jump(call_addr);
}

#ifdef RISCV_DEBUG

template <int W>
inline void Machine<W>::break_now()
{
	cpu.break_now();
}

#endif
