
template <int W>
inline void Machine<W>::stop() noexcept {
	m_max_counter = 0;
}
template <int W>
inline bool Machine<W>::stopped() const noexcept {
	return m_counter >= m_max_counter;
}
template <int W>
inline bool Machine<W>::instruction_limit_reached() const noexcept {
	return m_counter >= m_max_counter && m_max_counter != 0;
}

template <int W>
template <bool Throw>
inline void Machine<W>::simulate(uint64_t max_instr)
{
	cpu.simulate(max_instr);
	if constexpr (Throw) {
		// It is a timeout exception if the max counter is non-zero and
		// the simulation ended. Otherwise, the machine stopped normally.
		if (UNLIKELY(m_max_counter != 0))
			timeout_exception(max_instr);
	}
}

template <int W>
inline void Machine<W>::reset()
{
	cpu.reset();
	memory.reset();
}

template <int W>
inline void Machine<W>::print(const char* buffer, size_t len) const
{
	this->m_printer(buffer, len);
}
template <int W>
inline long Machine<W>::stdin_read(char* buffer, size_t len) const
{
	return this->m_stdin(buffer, len);
}
template <int W>
inline void Machine<W>::debug_print(const char* buffer, size_t len) const
{
	this->m_debug_printer(buffer, len);
}

template <int W> inline
void Machine<W>::install_syscall_handler(size_t sysn, syscall_t handler)
{
	syscall_handlers.at(sysn) = handler;
}
template <int W> inline
void Machine<W>::install_syscall_handlers(std::initializer_list<std::pair<size_t, syscall_t>> syscalls)
{
	for (auto& scall : syscalls)
		install_syscall_handler(scall.first, scall.second);
}

template <int W>
inline void Machine<W>::system_call(size_t sysnum)
{
	const auto& handler = Machine::syscall_handlers.at(sysnum);
	handler(*this);
}
template <int W>
inline void Machine<W>::unchecked_system_call(size_t syscall_number)
{
	const auto& handler = Machine::syscall_handlers[syscall_number];
	handler(*this);
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
void Machine<W>::copy_to_guest(address_t dst, const void* buf, size_t len)
{
	memory.memcpy(dst, buf, len);
}

template <int W>
void Machine<W>::copy_from_guest(void* dst, address_t buf, size_t len)
{
	memory.memcpy_out(dst, buf, len);
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
	sp = (sp - length) & ~(address_t) (W-1); // maintain word alignment
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
	static_assert(std::is_standard_layout_v<T>, "Must be a POD type");
	return stack_push(&type, sizeof(T));
}

template <int W>
void Machine<W>::realign_stack()
{
	// the RISC-V calling convention mandates a 16-byte alignment
	cpu.reg(REG_SP) &= ~(address_t) 0xF;
}

template <int W>
const FileDescriptors& Machine<W>::fds() const
{
	if (m_fds != nullptr) return *m_fds;
	throw MachineException(ILLEGAL_OPERATION, "No access to files or sockets", 0);
}
template <int W>
FileDescriptors& Machine<W>::fds()
{
	if (m_fds != nullptr) return *m_fds;
	throw MachineException(ILLEGAL_OPERATION, "No access to files or sockets", 0);
}

#include "machine_vmcall.hpp"
