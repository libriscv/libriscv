#pragma once
#include <libriscv/machine.hpp>
static constexpr bool verbose_syscalls = true;

template <int W>
uint32_t syscall_write(riscv::Machine<4>& machine)
{
	const int  fd      = machine.sysarg<int>(0);
	const auto address = machine.sysarg<uint32_t>(1);
	const auto len     = machine.sysarg<size_t>(2);
	if constexpr (false) {
		printf("SYSCALL write called, addr = %#X  len = %zu\n", address, len);
	}
	// we only accept standard pipes, for now :)
	if (fd >= 0 && fd < 3) {
		uint8_t buffer[len];
		machine.memory.memcpy_out(buffer, address, len);
		return write(fd, buffer, len);
	}
	return -1;
}

template <int W>
uint32_t syscall_close(riscv::Machine<W>& machine)
{
	const int fd = machine.template sysarg<int>(0);
	if constexpr (verbose_syscalls) {
		printf("SYSCALL closse called, fd = %d\n", fd);
	}
	if (fd <= 2) {
		return 0;
	}
	printf(">>> Close %d\n", fd);
	return -1;
}

template <int W>
uint32_t syscall_exit(riscv::Machine<W>& machine)
{
	printf(">>> Program exited, exit code = %d\n", machine.template sysarg<int> (0));
	machine.stop();
	return 0;
}

template <int W>
uint32_t syscall_ebreak(riscv::Machine<W>& machine)
{
	printf("\n>>> EBREAK at %#X", machine.cpu.pc());
#ifdef RISCV_DEBUG
	machine.break_now();
#else
	throw std::runtime_error("Unhandled EBREAK instruction");
#endif
	return 0;
}

template <int W>
uint32_t syscall_openat(riscv::Machine<W>&);

template <int W>
uint32_t syscall_readlinkat(riscv::Machine<W>&);

template <int W>
uint32_t syscall_writev(riscv::Machine<W>&);

template <int W>
uint32_t syscall_brk(riscv::Machine<W>&);

template <int W>
uint32_t syscall_mmap(riscv::Machine<W>&);

template <int W>
uint32_t syscall_stat(riscv::Machine<W>&);

template <int W>
uint32_t syscall_spm(riscv::Machine<W>&);

template <int W>
uint32_t syscall_uname(riscv::Machine<W>&);


template <int W>
static uint32_t syscall_geteuid(riscv::Machine<W>&) {
	return 0;
}
template <int W>
static uint32_t syscall_getuid(riscv::Machine<W>&) {
	return 0;
}
template <int W>
static uint32_t syscall_getegid(riscv::Machine<W>&) {
	return 0;
}
template <int W>
static uint32_t syscall_getgid(riscv::Machine<W>&) {
	return 0;
}

template <int W>
inline void add_linux_syscalls(riscv::Machine<W>& machine)
{
	machine.install_syscall_handler(56, syscall_openat<riscv::RISCV32>);
	machine.install_syscall_handler(57, syscall_close<riscv::RISCV32>);
	machine.install_syscall_handler(66, syscall_writev<riscv::RISCV32>);
	machine.install_syscall_handler(78, syscall_readlinkat<riscv::RISCV32>);
	machine.install_syscall_handler(80, syscall_stat<riscv::RISCV32>);
	machine.install_syscall_handler(135, syscall_spm<riscv::RISCV32>);
	machine.install_syscall_handler(160, syscall_uname<riscv::RISCV32>);
	machine.install_syscall_handler(174, syscall_getuid<riscv::RISCV32>);
	machine.install_syscall_handler(175, syscall_geteuid<riscv::RISCV32>);
	machine.install_syscall_handler(176, syscall_getgid<riscv::RISCV32>);
	machine.install_syscall_handler(177, syscall_getegid<riscv::RISCV32>);
	machine.install_syscall_handler(214, syscall_brk<riscv::RISCV32>);
	machine.install_syscall_handler(222, syscall_mmap<riscv::RISCV32>);
}
