#include <include/syscall_helpers.hpp>
#include <include/native_heap.hpp>
using namespace riscv;
//#define SYSPRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
static const uint64_t ARENA_BASE = 0x40000000;

#ifndef CUSTOM_NATIVE_SYSCALL_NUMBERS
static const int SYSCALL_MALLOC  = 1;
static const int SYSCALL_CALLOC  = 2;
static const int SYSCALL_FREE    = 4;

static const int SYSCALL_MEMCPY  = 5;
static const int SYSCALL_MEMSET  = 6;
static const int SYSCALL_MEMMOVE = 7;
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
template <int W>
void setup_native_memory_syscalls(Machine<W>& machine, bool trusted)
{
	machine.install_syscall_handler(SYSCALL_MEMCPY,
	[] (auto& m) -> long
	{
		const auto [dst, src, len] = 
			m.template sysargs<address_type<W>, address_type<W>, address_type<W>> ();
		SYSPRINT("SYSCALL memcpy(%#X, %#X, %u)\n", dst, src, len);
		for (size_t i = 0; i < len; i++) {
			m.memory.template write<uint8_t> (dst + i, 
				m.memory.template read<uint8_t> (src + i));
		}
		m.cpu.registers().counter += 2 * len;
		return dst;
	});
	machine.install_syscall_handler(SYSCALL_MEMSET,
	[] (auto& m) -> long
	{
		const auto [dst, value, len] = 
			m.template sysargs<address_type<W>, address_type<W>, address_type<W>> ();
		SYSPRINT("SYSCALL memset(%#X, %#X, %u)\n", dst, value, len);
		for (size_t i = 0; i < len; i++) {
			m.memory.template write<uint8_t> (dst + i, value);
		}
		m.cpu.registers().counter += len;
		return dst;
	});
	machine.install_syscall_handler(SYSCALL_MEMMOVE,
	[] (auto& m) -> long
	{
		auto [dst, src, len] = 
			m.template sysargs<address_type<W>, address_type<W>, address_type<W>> ();
		SYSPRINT("SYSCALL memmove(%#X, %#X, %u)\n", dst, src, len);
		if (src < dst)
		{
			unsigned i = 0;
			while (i++ != len) {
				m.memory.template write<uint8_t> (dst + i, 
					m.memory.template read<uint8_t> (src + i));
			}
		} else {
			while (len-- != 0) {
				m.memory.template write<uint8_t> (dst + len, 
					m.memory.template read<uint8_t> (src + len));
			}
		}
		m.cpu.registers().counter += 2 * len;
		return dst;
	});
}

/* le sigh */
template void setup_native_heap_syscalls<4>(Machine<4>&, size_t);
template void setup_native_memory_syscalls<4>(Machine<4>&, bool);
