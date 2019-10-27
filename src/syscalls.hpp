#pragma once
#include <libriscv/machine.hpp>

static constexpr bool verbose_syscalls = false;
static constexpr bool verbose_machine  = true;

template <int W>
uint32_t syscall_write(riscv::Machine<4>& machine)
{
	const int  fd      = machine.sysarg<int>(0);
	const auto address = machine.sysarg<uint32_t>(1);
	const auto len     = machine.sysarg<size_t>(2);
	if constexpr (verbose_syscalls) {
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
uint32_t syscall_stat(riscv::Machine<4>& machine)
{
	const auto  dirfd   = machine.sysarg<int>(0);
	const auto  buffer  = machine.sysarg<uint32_t>(1);
	if constexpr (verbose_syscalls) {
		printf("SYSCALL stat called, dirfd = %d  buffer = 0x%X\n",
				dirfd, buffer);
	}
	return -ENOSYS;
}

template <int W>
uint32_t syscall_close(riscv::Machine<W>& machine)
{
	const int fd = machine.template sysarg<int>(0);
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
	machine.break_now();
	return 0;
}
