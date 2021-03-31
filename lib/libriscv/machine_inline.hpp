
template <int W>
inline Machine<W>::Machine(std::string_view binary, MachineOptions<W> options)
	: cpu(*this), memory(*this, binary, options)
{
	cpu.reset();
}
template <int W>
inline Machine<W>::Machine(const Machine& other, MachineOptions<W> options)
	: cpu(*this, other), memory(*this, other, options)
{
	this->increment_counter(other.instruction_counter());
}

template <int W>
inline Machine<W>::Machine(const std::vector<uint8_t>& bin, MachineOptions<W> opts)
	: Machine(std::string_view{(char*) bin.data(), bin.size()}, opts) {}


template <int W>
inline void Machine<W>::stop(bool v) noexcept {
	m_stopped = v;
}
template <int W>
inline bool Machine<W>::stopped() const noexcept {
	return m_stopped;
}

template <int W>
template <bool Throw>
inline void Machine<W>::simulate(uint64_t max_instr)
{
	this->m_stopped = false;
	if (max_instr != 0) {
		const uint64_t max = m_counter + max_instr;
		while (m_counter < max)
		{
			cpu.simulate();
			this->m_counter ++;
			if (UNLIKELY(this->stopped()))
				return;
		}
		if constexpr (Throw) {
			throw MachineTimeoutException(MAX_INSTRUCTIONS_REACHED,
				"Instruction count limit reached", max_instr);
		}
	}
	else {
		uint64_t i = 0;
		while (LIKELY(!this->stopped())) {
			cpu.simulate();
			i++;
		}
		this->m_counter += i;
	}
}

template <int W>
inline void Machine<W>::reset()
{
	cpu.reset();
	memory.reset();
}

template <int W> inline
void Machine<W>::install_syscall_handler(int sysn, const syscall_t& handler)
{
	new (&m_syscall_handlers.at(sysn)) syscall_t(handler);
}
template <int W> inline
void Machine<W>::install_syscall_handlers(std::initializer_list<std::pair<int, syscall_t>> syscalls)
{
	for (auto& scall : syscalls)
		this->install_syscall_handler(scall.first, std::move(scall.second));
}
template <int W>
template <size_t N> inline
void Machine<W>::install_syscall_handler_range(int base, const std::array<const syscall_t, N>& syscalls)
{
	auto* first = &m_syscall_handlers.at(base);
	if (m_syscall_handlers.size() >= base + syscalls.size())
	{
		std::copy(syscalls.begin(), syscalls.end(), first);
	}
}
template <int W> inline
auto& Machine<W>::get_syscall_handler(int sysn) {
	return m_syscall_handlers.at(sysn);
}

template <int W>
inline void Machine<W>::system_call(size_t syscall_number)
{
	if (LIKELY(syscall_number < RISCV_SYSCALLS_MAX))
	{
		const auto& handler = m_syscall_handlers[syscall_number];
		if (LIKELY(handler != nullptr)) {
			handler(*this);
			return;
		}
	}
	unknown_syscall_handler(*this);
}
template <int W>
inline void Machine<W>::unchecked_system_call(size_t syscall_number)
{
	const auto& handler = m_syscall_handlers[syscall_number];
	if (LIKELY(handler != nullptr)) {
		handler(*this);
	} else {
		unknown_syscall_handler(*this);
	}
}

template <int W>
template <typename T>
inline T Machine<W>::sysarg(int idx) const
{
	if constexpr (std::is_integral_v<T>) {
		// 64-bit integers on 32-bit uses 2 registers
		if constexpr (sizeof(T) > W) {
			return static_cast<T> (cpu.reg(REG_ARG0 + idx))
				| static_cast<T> (cpu.reg(REG_ARG0 + idx + 1)) << 32;
		}
		return static_cast<T> (cpu.reg(REG_ARG0 + idx));
	}
	else if constexpr (std::is_same_v<T, float>)
		return cpu.registers().getfl(REG_FA0 + idx).f32[0];
	else if constexpr (std::is_same_v<T, double>)
		return cpu.registers().getfl(REG_FA0 + idx).f64;
	else if constexpr (std::is_same_v<T, riscv::Buffer>)
		return memory.rvbuffer(
			cpu.reg(REG_ARG0 + idx), cpu.reg(REG_ARG0 + idx + 1));
	else if constexpr (is_stdstring<T>::value)
		return memory.memstring(cpu.reg(REG_ARG0 + idx));
	else if constexpr (std::is_pod_v<std::remove_reference<T>>) {
		T value;
		memory.memcpy_out(&value, cpu.reg(REG_ARG0 + idx), sizeof(T));
		return value;
	} else
		static_assert(always_false<T>, "Unknown type");
}

template <int W>
template<typename... Args, std::size_t... Indices>
inline auto Machine<W>::resolve_args(std::index_sequence<Indices...>) const
{
	std::tuple<std::decay_t<Args>...> retval;
	size_t i = 0;
	size_t f = 0;
	([&] {
		if constexpr (std::is_integral_v<Args>) {
			std::get<Indices>(retval) = sysarg<Args>(i++);
			if constexpr (sizeof(Args) > W) i++; // uses 2 registers
		}
		else if constexpr (std::is_floating_point_v<Args>)
			std::get<Indices>(retval) = sysarg<Args>(f++);
		else if constexpr (std::is_same_v<Args, riscv::Buffer>) {
			std::get<Indices>(retval) = std::move(sysarg<Args>(i)); i += 2; // ptr, len
		}
		else if constexpr (is_stdstring<Args>::value)
			std::get<Indices>(retval) = sysarg<Args>(i++);
		else if constexpr (std::is_pod_v<std::remove_reference<Args>>)
			std::get<Indices>(retval) = sysarg<Args>(i++);
		else
			static_assert(always_false<Args>, "Unknown type");
	}(), ...);
	return retval;
}

template <int W>
template<typename... Args>
inline auto Machine<W>::sysargs() const {
	return resolve_args<Args...>(std::index_sequence_for<Args...>{});
}

template <int W>
template <typename... Args>
inline void Machine<W>::set_result(Args... args) {
	size_t i = 0;
	size_t f = 0;
	([&] {
		if constexpr (std::is_integral_v<Args>) {
			cpu.registers().at(REG_ARG0 + i++) = args;
		}
		else if constexpr (std::is_same_v<Args, float>)
			cpu.registers().getfl(REG_FA0 + f++).set_float(args);
		else if constexpr (std::is_same_v<Args, double>)
			cpu.registers().getfl(REG_FA0 + f++).set_double(args);
		else
			static_assert(always_false<Args>, "Unknown type");
	}(), ...);
}

template <int W>
inline void Machine<W>::ebreak()
{
#ifdef RISCV_EBREAK_MEANS_STOP
	this->stop();
#else
	// its simpler and more flexible to just call a user-provided function
	this->system_call(riscv::SYSCALL_EBREAK);
#endif
}

template <int W>
address_type<W> Machine<W>::copy_to_guest(address_t dst, const void* buf, size_t len)
{
	memory.memcpy_unsafe(dst, buf, len);
	return dst + len;
}

template <int W>
inline address_type<W> Machine<W>::address_of(const char* name) const {
	return memory.resolve_address(name);
}
template <int W>
inline address_type<W> Machine<W>::address_of(const std::string& name) const {
	return memory.resolve_address(name.c_str());
}

template <int W>
address_type<W> Machine<W>::stack_push(const void* data, size_t length)
{
	auto& sp = cpu.reg(REG_SP);
	sp = (sp - length) & ~(W-1); // maintain word alignment
	this->copy_to_guest(sp, data, length);
	return sp;
}
template <int W>
address_type<W> Machine<W>::stack_push(const std::string& string)
{
	return stack_push(string.data(), string.size()+1); /* zero */
}
template <int W>
template <typename T>
address_type<W> Machine<W>::stack_push(const T& type)
{
	return stack_push(&type, sizeof(T));
}

template <int W>
void Machine<W>::realign_stack()
{
	// the RISC-V calling convention mandates a 16-byte alignment
	cpu.reg(REG_SP) &= ~(address_t) 0xF;
}

template <int W>
inline void Machine<W>::add_destructor_callback(Function<void()> cb) const
{
	m_destructor_callbacks.push_back(std::move(cb));
}

#include "machine_vmcall.hpp"
