#pragma once

template<typename T>
struct is_string
	: public std::disjunction<
		std::is_same<char *, typename std::decay<T>::type>,
		std::is_same<const char *, typename std::decay<T>::type>,
		std::is_same<std::string, typename std::decay<T>::type>
> {};

template <int W>
template <typename... Args> constexpr
inline void Machine<W>::setup_call(
		address_t call_addr, address_t retn_addr, Args&&... args)
{
	cpu.reg(RISCV::REG_RA) = retn_addr;
	int iarg = RISCV::REG_ARG0;
	int farg = RISCV::REG_FA0;
	([&] {
		if constexpr (std::is_integral_v<Args>)
			cpu.reg(iarg++) = args;
		else if constexpr (is_string<Args>::value)
			cpu.reg(iarg++) = stack_push(args, strlen(args)+1);
		else
			cpu.registers().getfl(farg++).set_float(args);
	}(), ...);
	cpu.jump(call_addr);
}

template <int W>
template <uint64_t MAXI, typename... Args>
inline long Machine<W>::vmcall(const char* function_name, Args&&... args)
{
	address_t call_addr = memory.resolve_address(function_name);
	address_t retn_addr = memory.exit_address();
	const address_t sp = cpu.reg(RISCV::REG_SP);

	this->setup_call(call_addr, retn_addr, std::forward<Args>(args)...);
	this->simulate(MAXI);
	// restore stack pointer
	this->cpu.reg(RISCV::REG_SP) = sp;
	return this->sysarg<address_t> (0);
}
