#include <include/syscall_helpers.hpp>
#include <include/native_heap.hpp>
using namespace riscv;
//#define SYSPRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
static const uint64_t ARENA_BASE = 0x40000000;

#ifndef CUSTOM_NATIVE_SYSCALL_NUMBERS
static const uint32_t SYSCALL_MALLOC  = 1;
static const uint32_t SYSCALL_CALLOC  = 2;
static const uint32_t SYSCALL_FREE    = 3;
#endif

template <int W>
void setup_native_heap_syscalls(Machine<W>& machine, size_t max_memory)
{
	auto* arena = new sas_alloc::Arena(ARENA_BASE, ARENA_BASE + max_memory);
	machine.add_destructor_callback([arena] { delete arena; });

	machine.install_syscall_handler(SYSCALL_MALLOC,
	[arena] (auto& machine) -> long
	{
		const size_t len = machine.template sysarg<address_type<W>>(0);
		auto data = arena->malloc(len);
		SYSPRINT("SYSCALL malloc(%zu) = 0x%X\n", len, data);
		return data;
	});
	machine.install_syscall_handler(SYSCALL_CALLOC,
	[arena] (auto& machine) -> long
	{
		const auto [count, size] = 
			machine.template sysargs<address_type<W>, address_type<W>> ();
		const size_t len = count * size;
		auto data = arena->malloc(len);
		SYSPRINT("SYSCALL calloc(%zu, %zu) = 0x%X\n", count, size, data);
		if (data != 0) {
			// TODO: optimize this (CoW), **can throw**
			machine.memory.memset(data, 0, len);
		}
		return data;
	});
	machine.install_syscall_handler(SYSCALL_FREE,
	[arena] (auto& machine) -> long
	{
		const auto ptr = machine.template sysarg<address_type<W>>(0);
		int ret = arena->free(ptr);
		SYSPRINT("SYSCALL free(0x%X) = %d\n", ptr, ret);
		return ret;
	});
}

/* le sigh */
template void setup_native_heap_syscalls<4>(Machine<4>&, size_t);
