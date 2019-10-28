#pragma once
#include <libriscv/machine.hpp>

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
uint32_t syscall_brk(riscv::Machine<4>& machine)
{
	static const uint32_t heap_start = 0xA0000000;
	static const uint32_t heap_max   = 0xF0000000;
	static uint32_t heap_end = heap_start;
	const int32_t incr = machine.sysarg<int32_t>(0);
	if constexpr (verbose_syscalls) {
		printf("SYSCALL brk called, heap_end = 0x%X incr = 0x%X\n", heap_end, incr);
	}
	if (heap_end + incr > heap_max) {
		return 0;
	}
	auto heap_old_end = heap_end;
	heap_end += incr;
	printf("New heap end: 0x%X\n", heap_end);
	return heap_end;
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
