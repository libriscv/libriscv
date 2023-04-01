
template <int W>
template <typename... Args> constexpr
inline void Machine<W>::setup_call(address_t call_addr, Args&&... args)
{
	cpu.reg(REG_RA) = memory.exit_address();
	[[maybe_unused]] int iarg = REG_ARG0;
	[[maybe_unused]] int farg = REG_FA0;
	([&] {
		if constexpr (std::is_integral_v<remove_cvref<Args>>) {
			cpu.reg(iarg++) = args;
			if constexpr (sizeof(Args) > W) // upper 32-bits for 64-bit integers
				cpu.reg(iarg++) = args >> 32;
		}
		else if constexpr (is_stdstring<remove_cvref<Args>>::value)
			cpu.reg(iarg++) = stack_push(args.data(), args.size()+1);
		else if constexpr (is_string<Args>::value)
			cpu.reg(iarg++) = stack_push(args, strlen(args)+1);
		else if constexpr (std::is_floating_point_v<remove_cvref<Args>>)
			cpu.registers().getfl(farg++).set_float(args);
		else if constexpr (std::is_standard_layout_v<remove_cvref<Args>> && std::is_trivial_v<remove_cvref<Args>>)
			cpu.reg(iarg++) = stack_push(&args, sizeof(args));
		else
			static_assert(always_false<decltype(args)>, "Unknown type");
	}(), ...);
	cpu.reg(REG_SP) &= ~address_t(0xF);
	cpu.jump(call_addr);
}

template <int W>
template <uint64_t MAXI, bool Throw, typename... Args> constexpr
inline address_type<W> Machine<W>::vmcall(address_t call_addr, Args&&... args)
{
	// reset the stack pointer to an initial location (deliberately)
	this->cpu.reset_stack_pointer();
	// setup calling convention
	this->setup_call(call_addr, std::forward<Args>(args)...);
	// execute function
	if (MAXI != 0)
		this->simulate<Throw>(MAXI);
	else
		this->simulate<Throw>(max_instructions());
	// address-sized integer return value
	return cpu.reg(REG_ARG0);
}

template <int W>
template <uint64_t MAXI, bool Throw, typename... Args> constexpr
inline address_type<W> Machine<W>::vmcall(const char* funcname, Args&&... args)
{
	address_t call_addr = memory.resolve_address(funcname);
	return vmcall<MAXI>(call_addr, std::forward<Args>(args)...);
}

template <int W>
template <uint64_t MAXI, bool Throw, bool StoreRegs, typename... Args> inline
address_type<W> Machine<W>::preempt(address_t call_addr, Args&&... args)
{
	Registers<W> regs;
	if constexpr (StoreRegs) {
		regs = cpu.registers();
	}
	const uint64_t max_counter = this->max_instructions();
	// we need to make some stack room
	this->cpu.reg(REG_SP) -= 16u;
	// setup calling convention
	this->setup_call(call_addr, std::forward<Args>(args)...);
	this->realign_stack();
	// execute function
	try {
		if (MAXI != 0)
			this->simulate<Throw>(MAXI);
		else
			this->simulate<Throw>(max_instructions());
	} catch (...) {
		this->m_max_counter = max_counter;
		if constexpr (StoreRegs) {
			cpu.registers() = regs;
			cpu.aligned_jump(cpu.pc());
		}
		throw;
	}
	// restore registers and return value
	this->m_max_counter = max_counter;
	const auto retval = cpu.reg(REG_ARG0);
	if constexpr (StoreRegs) {
		cpu.registers() = regs;
		cpu.aligned_jump(cpu.pc());
	}
	return retval;
}

template <int W>
template <uint64_t MAXI, bool Throw, bool StoreRegs, typename... Args> inline
address_type<W> Machine<W>::preempt(const char* funcname, Args&&... args)
{
	address_t call_addr = memory.resolve_address(funcname);
	return preempt<MAXI, Throw, StoreRegs>(call_addr, std::forward<Args>(args)...);
}
