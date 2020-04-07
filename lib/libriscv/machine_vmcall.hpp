#pragma once

template <int W>
template <typename... Args> constexpr
inline void Machine<W>::setup_call(address_t call_addr, Args&&... args)
{
	cpu.reg(RISCV::REG_RA) = memory.exit_address();
	int iarg = RISCV::REG_ARG0;
	int farg = RISCV::REG_FA0;
	([&] {
		if constexpr (std::is_integral_v<Args>)
			cpu.reg(iarg++) = args;
		else if constexpr (is_stdstring<Args>::value)
			cpu.reg(iarg++) = stack_push(args.data(), args.size()+1);
		else if constexpr (is_string<Args>::value)
			cpu.reg(iarg++) = stack_push(args, strlen(args)+1);
		else if constexpr (std::is_floating_point_v<Args>)
			cpu.registers().getfl(farg++).set_float(args);
		else if constexpr (std::is_pod_v<std::remove_reference<Args>>)
			cpu.reg(iarg++) = stack_push(&args, sizeof(args));
		else
			static_assert(always_false<decltype(args)>, "Unknown type");
	}(), ...);
	cpu.jump(call_addr);
}

template <int W>
template <uint64_t MAXI, typename... Args> constexpr
inline address_type<W> Machine<W>::vmcall(address_t call_addr, Args&&... args)
{
	const address_t sp = cpu.reg(RISCV::REG_SP);
	// setup calling convention
	this->setup_call(call_addr, std::forward<Args>(args)...);
	// execute function
	this->simulate(MAXI);
	// restore stack pointer
	this->cpu.reg(RISCV::REG_SP) = sp;
	// address-sized integer return value
	return cpu.reg(RISCV::REG_ARG0);
}

template <int W>
template <uint64_t MAXI, typename... Args> constexpr
inline address_type<W> Machine<W>::vmcall(const char* funcname, Args&&... args)
{
	address_t call_addr = memory.resolve_address(funcname);
	return vmcall<MAXI>(call_addr, std::forward<Args>(args)...);
}
