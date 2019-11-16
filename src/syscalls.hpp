#pragma once
#include <libriscv/machine.hpp>
static constexpr bool verbose_syscalls = false;

//#define SYSCALL_VERBOSE 1
#ifdef SYSCALL_VERBOSE
#define SYSPRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define SYSPRINT(fmt, ...) /* fmt */
#endif

template <int W> inline
uint32_t syscall_write(riscv::Machine<W>& machine);

template <int W>
uint32_t syscall_close(riscv::Machine<W>& machine)
{
	const int fd = machine.template sysarg<int>(0);
	if constexpr (verbose_syscalls) {
		printf("SYSCALL close called, fd = %d\n", fd);
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
	const int status = machine.template sysarg<int> (0);
	printf(">>> Program exited, exit code = %d\n", status);
	machine.stop();
	return status;
}

template <int W>
uint32_t syscall_ebreak(riscv::Machine<W>& machine)
{
	printf("\n>>> EBREAK at %#X\n", machine.cpu.pc());
#ifdef RISCV_DEBUG
	machine.print_and_pause();
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
inline void add_newlib_syscalls(riscv::Machine<W>& machine)
{
	machine.install_syscall_handler(214, syscall_brk<riscv::RISCV32>);
	machine.install_syscall_handler(222, syscall_mmap<riscv::RISCV32>);
}

template <int W>
void setup_linux_syscalls(riscv::Machine<W>&);
