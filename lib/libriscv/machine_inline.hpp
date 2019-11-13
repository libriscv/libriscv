
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
		if (syscall_number != EBREAK_SYSCALL) {
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
								std::initializer_list<address_t> args,
								uint64_t max_instructions)
{
	address_t call_addr = memory.resolve_address(function_name);
	address_t retn_addr = memory.resolve_address("_exit");
	this->setup_call(call_addr, retn_addr, std::move(args));
	long retval = -1;
	this->install_syscall_handler(93,
		[&retval] (auto& machine) -> long {
			retval = machine.template sysarg<long> (0);
			machine.stop();
			// Since the return value of this system call will overwrite its own
			// argument, we will just return the argument itself.
			return retval;
		});
	this->simulate(max_instructions);
	return retval;
}


template <int W>
inline void Machine<W>::setup_call(
		address_t call_addr, address_t retn_addr,
		std::initializer_list<address_t> args)
{
	assert(args.size() <= 8);
	cpu.reg(RISCV::REG_RA) = retn_addr;
	size_t arg = 0;
	for (auto value : args) {
		cpu.reg(RISCV::REG_ARG0 + arg) = value;
		arg++;
	}
	cpu.jump(call_addr);
}

template <int W>
inline address_type<W> Machine<W>::address_of(const std::string& name)
{
	return memory.resolve_address(name);
}

template <int W>
void Machine<W>::realign_stack(uint8_t align)
{
	address_t align_mask = 15;
	switch (align) {
		case 4:  align_mask = 0x3; break;
		case 8:  align_mask = 0x7; break;
		case 16: align_mask = 0xF; break;
		default: throw std::runtime_error("Invalid alignment");
	}
	cpu.reg(RISCV::REG_SP) &= ~align_mask;
}
