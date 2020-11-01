#include <include/syscall_helpers.hpp>
#include <include/native_heap.hpp>
using namespace riscv;
using namespace sas_alloc;
//#define SYSPRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
static const uint64_t ARENA_BASE = 0x40000000;

#ifndef NATIVE_SYSCALLS_BASE
#define NATIVE_SYSCALLS_BASE    1  /* They start at 1 */
#endif

template <int W> address_type<W> machine_memcpy(
	Machine<W>& m, address_type<W> dst, address_type<W> src, address_type<W> len)
{
	if ((dst & (W-1)) == (src & (W-1))) {
		while ((src & (W-1)) != 0 && len > 0) {
			m.memory.template write<uint8_t> (dst++,
				m.memory.template read<uint8_t> (src++));
			len --;
		}
		while (len >= 16) {
			m.memory.template write<uint32_t> (dst + 0,
				m.memory.template read<uint32_t> (src + 0));
			m.memory.template write<uint32_t> (dst + 1*W,
				m.memory.template read<uint32_t> (src + 1*W));
			m.memory.template write<uint32_t> (dst + 2*W,
				m.memory.template read<uint32_t> (src + 2*W));
			m.memory.template write<uint32_t> (dst + 3*W,
				m.memory.template read<uint32_t> (src + 3*W));
			dst += 16; src += 16; len -= 16;
		}
		while (len >= W) {
			m.memory.template write<uint32_t> (dst,
				m.memory.template read<uint32_t> (src));
			dst += W; src += W; len -= W;
		}
	}
	while (len > 0) {
		m.memory.template write<uint8_t> (dst++,
			m.memory.template read<uint8_t> (src++));
		len --;
	}
	return dst;
}

template <int W>
static void setup_native_heap_syscalls(Machine<W>& machine, 
	sas_alloc::Arena* arena)
{
	// Malloc n+0
	machine.install_syscall_handler(NATIVE_SYSCALLS_BASE+0,
	[arena] (auto& machine)
	{
		const size_t len = machine.template sysarg<address_type<W>>(0);
		auto data = arena->malloc(len);
		SYSPRINT("SYSCALL malloc(%zu) = 0x%X\n", len, data);
		machine.set_result(data);
	});
	// Calloc n+1
	machine.install_syscall_handler(NATIVE_SYSCALLS_BASE+1,
	[arena] (auto& machine)
	{
		const auto [count, size] = 
			machine.template sysargs<address_type<W>, address_type<W>> ();
		const size_t len = count * size;
		auto data = arena->malloc(len);
		SYSPRINT("SYSCALL calloc(%u, %u) = 0x%X\n", count, size, data);
		if (data != 0) {
			// TODO: optimize this (CoW), **can throw**
			machine.memory.memset(data, 0, len);
		}
		machine.set_result(data);
	});
	// Realloc n+2
	machine.install_syscall_handler(NATIVE_SYSCALLS_BASE+2,
	[arena] (auto& machine)
	{
		const address_type<W> src = machine.template sysarg<address_type<W>>(0);
		const size_t newlen = machine.template sysarg<address_type<W>>(1);
		if (src != 0)
		{
			const size_t srclen = arena->size(src);
			if (srclen > 0)
			{
				// XXX: really have to know what we are doing here
				// we are freeing in the hopes of getting the same chunk
				// back, in which case the copy could be completely skipped.
				arena->free(src);
				auto data = arena->malloc(newlen);
				SYSPRINT("SYSCALL realloc(0x%X:%zu, %zu) = 0x%X)\n", src, srclen, newlen, data);
				if (data != 0 && data != src)
				{
					machine_memcpy(machine, data, src, srclen);
				}
				machine.set_result(data);
				return;
			} else {
				SYSPRINT("SYSCALL realloc(0x%X:??, %zu) = 0x0\n", src, newlen);
			}
		} else {
			auto data = arena->malloc(newlen);
			SYSPRINT("SYSCALL realloc(0x0, %zu) = 0x%X)\n", newlen, data);
			machine.set_result(data);
			return;
		}
		machine.set_result(0);
	});
	// Free n+3
	machine.install_syscall_handler(NATIVE_SYSCALLS_BASE+3,
	[arena] (auto& machine)
	{
		const auto ptr = machine.template sysarg<address_type<W>>(0);
		if (ptr != 0)
		{
			int ret = arena->free(ptr);
			SYSPRINT("SYSCALL free(0x%X) = %d\n", ptr, ret);
			machine.set_result(ret);
			return;
		}
		SYSPRINT("SYSCALL free(0x0) = 0\n");
		machine.set_result(0);
		return;
	});
	// Meminfo n+4
	machine.install_syscall_handler(NATIVE_SYSCALLS_BASE+4,
	[arena] (auto& machine)
	{
		const auto dst = machine.template sysarg<address_type<W>>(0);
		struct Result {
			const uint32_t bf;
			const uint32_t bu;
			const uint32_t cu;
		} result = {
			.bf = (uint32_t) arena->bytes_free(),
			.bu = (uint32_t) arena->bytes_used(),
			.cu = (uint32_t) arena->chunks_used()
		};
		int ret = (dst != 0) ? 0 : -1;
		SYSPRINT("SYSCALL meminfo(0x%X) = %d\n", dst, ret);
		if (ret == 0) {
			machine.copy_to_guest(dst, &result, sizeof(result));
		}
		machine.set_result(ret);
	});
}

template <int W>
Arena* setup_native_heap_syscalls(Machine<W>& machine, size_t max_memory)
{
	auto* arena = new sas_alloc::Arena(ARENA_BASE, ARENA_BASE + max_memory);
	machine.add_destructor_callback([arena] { delete arena; });

	setup_native_heap_syscalls<W> (machine, arena);
	return arena;
}
template <int W>
Arena* setup_native_heap_syscalls(Machine<W>& machine, size_t max_memory,
	Function<void* (size_t)> constructor)
{
	sas_alloc::Arena* arena =
		(sas_alloc::Arena*) constructor(sizeof(sas_alloc::Arena));
	new (arena) sas_alloc::Arena(ARENA_BASE, ARENA_BASE + max_memory);

	setup_native_heap_syscalls<W> (machine, arena);
	return arena;
}

template <int W>
void setup_native_memory_syscalls(Machine<W>& machine, bool trusted)
{
	if (trusted == false)
	{
		// Memcpy n+5
		machine.install_syscall_handler(NATIVE_SYSCALLS_BASE+5,
		[] (auto& m)
		{
			auto [dst, src, len] =
				m.template sysargs<address_type<W>, address_type<W>, address_type<W>> ();
			SYSPRINT("SYSCALL memcpy(%#X, %#X, %u)\n", dst, src, len);
			m.cpu.increment_counter(2 * len);
			m.set_result(machine_memcpy(m, dst, src, len));
		});
		// Memset n+6
		machine.install_syscall_handler(NATIVE_SYSCALLS_BASE+6,
		[] (auto& m)
		{
			const auto [dst, value, len] = 
				m.template sysargs<address_type<W>, address_type<W>, address_type<W>> ();
			SYSPRINT("SYSCALL memset(%#X, %#X, %u)\n", dst, value, len);
			for (size_t i = 0; i < len; i++) {
				m.memory.template write<uint8_t> (dst + i, value);
			}
			m.cpu.increment_counter(len);
			m.set_result(dst);
		});
		// Memcmp n+8
		machine.install_syscall_handler(NATIVE_SYSCALLS_BASE+8,
		[] (auto& m)
		{
			auto [p1, p2, len] = 
				m.template sysargs<address_type<W>, address_type<W>, address_type<W>> ();
			SYSPRINT("SYSCALL memcmp(%#X, %#X, %u)\n", p1, p2, len);
			m.cpu.increment_counter(2 * len);
			uint8_t v1 = 0;
			uint8_t v2 = 0;
			while (len > 0) {
				v1 = m.memory.template read<uint8_t> (p1);
				v2 = m.memory.template read<uint8_t> (p2);
				if (v1 != v2) break;
				p1++;
				p2++;
				len--;
			}
			m.set_result(len == 0 ? 0 : (v1 - v2));
		});
		// Strlen n+10
		machine.install_syscall_handler(NATIVE_SYSCALLS_BASE+10,
		[] (auto& m)
		{
			auto [addr] = m.template sysargs<address_type<W>> ();
			SYSPRINT("SYSCALL strlen(%#X)\n", addr);
			address_type<W> iter = addr;
			do {
				auto v1 = m.memory.template read<uint8_t> (iter ++);
				if (v1 == 0) break;
			} while ((iter - addr) < 4096);
			const auto len = iter - addr;
			m.cpu.increment_counter(2 * len);
			m.set_result(len);
		});
	} else {
		/// trusted system calls ///
		// Memcpy n+5
		machine.install_syscall_handler(NATIVE_SYSCALLS_BASE+5,
		[] (auto& m)
		{
			auto [dst, src, len] =
				m.template sysargs<address_type<W>, address_type<W>, address_type<W>> ();
			SYSPRINT("SYSCALL memcpy(%#X, %#X, %u)\n", dst, src, len);
			m.cpu.increment_counter(2 * len);
			m.memory.memview(src, len,
				[&m] (const uint8_t* data, size_t len) {
					auto dst = m.template sysarg <address_type<W>> (0);
					m.memory.memcpy(dst, data, len);
				});
				m.set_result(dst);
		});
		// Memset n+6
		machine.install_syscall_handler(NATIVE_SYSCALLS_BASE+6,
		[] (auto& m)
		{
			const auto [dst, value, len] = 
				m.template sysargs<address_type<W>, address_type<W>, address_type<W>> ();
			SYSPRINT("SYSCALL memset(%#X, %#X, %u)\n", dst, value, len);
			m.memory.memset(dst, value, len);
			m.cpu.increment_counter(len);
			m.set_result(dst);
		});
		// Memcmp n+8
		machine.install_syscall_handler(NATIVE_SYSCALLS_BASE+8,
		[] (auto& m)
		{
			auto [p1, p2, len] = 
				m.template sysargs<address_type<W>, address_type<W>, address_type<W>> ();
			SYSPRINT("SYSCALL memcmp(%#X, %#X, %u)\n", p1, p2, len);
			m.cpu.increment_counter(2 * len);
			m.set_result(m.memory.memcmp(p1, p2, len));
		});
		// Strlen n+10
		machine.install_syscall_handler(NATIVE_SYSCALLS_BASE+10,
		[] (auto& m)
		{
			auto [addr] = m.template sysargs<address_type<W>> ();
			SYSPRINT("SYSCALL strlen(%#X)\n", addr);
			uint32_t len = m.memory.strlen(addr, 4096);
			m.cpu.increment_counter(2 * len);
			m.set_result(len);
		});
	} // trusted

	// Memmove n+7
	machine.install_syscall_handler(NATIVE_SYSCALLS_BASE+7,
	[] (auto& m)
	{
		auto [dst, src, len] = 
			m.template sysargs<address_type<W>, address_type<W>, address_type<W>> ();
		SYSPRINT("SYSCALL memmove(%#X, %#X, %u)\n", dst, src, len);
		if (src < dst)
		{
			for (unsigned i = 0; i != len; i++) {
				m.memory.template write<uint8_t> (dst + i, 
					m.memory.template read<uint8_t> (src + i));
			}
		} else {
			while (len-- != 0) {
				m.memory.template write<uint8_t> (dst + len, 
					m.memory.template read<uint8_t> (src + len));
			}
		}
		m.cpu.increment_counter(2 * len);
		m.set_result(dst);
	});

	// Strncmp n+11
	machine.install_syscall_handler(NATIVE_SYSCALLS_BASE+11,
	[] (auto& m)
	{
		auto [a1, a2, maxlen] =
			m.template sysargs<address_type<W>, address_type<W>, uint32_t> ();
		SYSPRINT("SYSCALL strncmp(%#lX, %#lX, %u)\n", (long)a1, (long)a2, maxlen);
		uint32_t len = 0;
		while (len < maxlen) {
			const uint8_t v1 = m.memory.template read<uint8_t> (a1 ++);
			const uint8_t v2 = m.memory.template read<uint8_t> (a2 ++);
			if (v1 != v2 || v1 == 0) {
				m.cpu.increment_counter(2 + 2 * len);
				m.set_result(v1 - v2);
				return;
			}
			len ++;
		}
		m.cpu.increment_counter(2 + 2 * len);
		m.set_result(0);
	});

	// Print backtrace n+19
	machine.install_syscall_handler(NATIVE_SYSCALLS_BASE+19,
	[] (auto& m)
	{
		m.memory.print_backtrace(
			[] (const char* buffer, size_t len) {
				printf("%.*s\n", (int)len, buffer);
			});
		m.set_result(0);
	});
}

uint64_t arena_malloc(sas_alloc::Arena* arena, const size_t len)
{
	return arena->malloc(len);
}

void arena_transfer(const sas_alloc::Arena* from, sas_alloc::Arena* to)
{
	from->transfer(*to);
}

/* le sigh */
template Arena* setup_native_heap_syscalls<4>(Machine<4>&, size_t);
template Arena* setup_native_heap_syscalls<4>(Machine<4>& machine, size_t, Function<void* (size_t)>);
template void setup_native_memory_syscalls<4>(Machine<4>&, bool);

template Arena* setup_native_heap_syscalls<8>(Machine<8>&, size_t);
template Arena* setup_native_heap_syscalls<8>(Machine<8>& machine, size_t, Function<void* (size_t)>);
template void setup_native_memory_syscalls<8>(Machine<8>&, bool);
