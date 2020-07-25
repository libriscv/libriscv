#pragma once
#include <libriscv/machine.hpp>
static constexpr bool verbose_syscalls = false;

//#define SYSCALL_VERBOSE 1
#ifdef SYSCALL_VERBOSE
#define SYSPRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define SYSPRINT(fmt, ...) /* fmt */
#endif

template <int W>
struct State
{
	int exit_code = 0;
	std::string output;

	long syscall_exit(riscv::Machine<W>&);
	long syscall_write(riscv::Machine<W>&);
	long syscall_writev(riscv::Machine<W>&);
};

template <int W>
void setup_minimal_syscalls(State<W>&, riscv::Machine<W>&);

template <int W>
void setup_newlib_syscalls(State<W>&, riscv::Machine<W>&);

template <int W>
void setup_linux_syscalls(State<W>&, riscv::Machine<W>&);

namespace sas_alloc {
	struct Arena;
}

template <int W>
sas_alloc::Arena* setup_native_heap_syscalls(riscv::Machine<W>&, size_t);
template <int W>
sas_alloc::Arena* setup_native_heap_syscalls(riscv::Machine<W>&, size_t, riscv::Function<void* (size_t)>);

template <int W>
void setup_native_memory_syscalls(riscv::Machine<W>&, bool trusted);

extern uint64_t arena_malloc(sas_alloc::Arena*, size_t len);
extern void arena_transfer(const sas_alloc::Arena* from, sas_alloc::Arena* to);

#include "threads.hpp"
