#include "machine.hpp"
#include "native_heap.hpp"

//#define VERBOSE_NATSYS
#ifdef VERBOSE_NATSYS
#define HPRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
#define MPRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define HPRINT(fmt, ...) /* */
#define MPRINT(fmt, ...) /* */
#endif

namespace riscv {
	// An arbitrary maximum length just to stop *somewhere*
	static constexpr size_t   STRLEN_MAX = 64'000u;
	static constexpr uint64_t COMPLEX_CALL_PENALTY = 2'000u;

template <int W>
void Machine<W>::setup_native_heap_internal(const size_t syscall_base)
{
	// Malloc n+0
	this->install_syscall_handler(syscall_base+0,
	[] (auto& machine)
	{
		const size_t len = machine.template sysarg<address_type<W>>(0);
		auto data = machine.arena().malloc(len);
		HPRINT("SYSCALL malloc(%zu) = 0x%lX\n", len, (long)data);
		machine.set_result(data);
		machine.penalize(COMPLEX_CALL_PENALTY);
	});
	// Calloc n+1
	this->install_syscall_handler(syscall_base+1,
	[] (auto& machine)
	{
		const auto [count, size] =
			machine.template sysargs<address_type<W>, address_type<W>> ();
		const size_t len = count * size;
		auto data = machine.arena().malloc(len);
		HPRINT("SYSCALL calloc(%zu, %zu) = 0x%lX\n",
			(size_t)count, (size_t)size, (long)data);
		if (data != 0) {
			// Optimized to skip zero pages
			machine.memory.memzero(data, len);
		}
		machine.set_result(data);
		machine.penalize(COMPLEX_CALL_PENALTY);
	});
	// Realloc n+2
	this->install_syscall_handler(syscall_base+2,
	[] (auto& machine)
	{
		const auto src = machine.sysarg(0);
		const auto newlen = machine.sysarg(1);

		const auto [data, srclen] = machine.arena().realloc(src, newlen);
		HPRINT("SYSCALL realloc(0x%lX:%zu, %zu) = 0x%lX\n",
			(long)src, (size_t)srclen, (size_t)newlen, (long)data);
		// When data != src, srclen is the old length, and the
		// chunks are non-overlapping, so we can use forwards memcpy.
		if (data != src && srclen != 0) {
			machine.memory.memcpy(data, machine, src, srclen);
			machine.penalize(2 * srclen);
		}
		machine.set_result(data);
		machine.penalize(COMPLEX_CALL_PENALTY);
	});
	// Free n+3
	this->install_syscall_handler(syscall_base+3,
	[] (auto& machine)
	{
		const auto ptr = machine.sysarg(0);
		if (ptr != 0)
		{
			int ret = machine.arena().free(ptr);
			HPRINT("SYSCALL free(0x%lX) = %d\n", (long)ptr, ret);
			machine.set_result(ret);
			if (ptr != 0x0 && ret < 0) {
				throw MachineException(SYSTEM_CALL_FAILED, "Possible double-free for freed pointer", ptr);
			}
			machine.penalize(COMPLEX_CALL_PENALTY);
			return;
		}
		HPRINT("SYSCALL free(0x0) = 0\n");
		machine.set_result(0);
		machine.penalize(COMPLEX_CALL_PENALTY);
		return;
	});
	// Meminfo n+4
	this->install_syscall_handler(syscall_base+4,
	[] (auto& machine)
	{
		const auto dst = machine.sysarg(0);
		const auto& arena = machine.arena();
		struct Result {
			const address_type<W> bf;
			const address_type<W> bu;
			const address_type<W> cu;
		} result = {
			.bf = (address_type<W>) arena.bytes_free(),
			.bu = (address_type<W>) arena.bytes_used(),
			.cu = (address_type<W>) arena.chunks_used()
		};
		int ret = (dst != 0) ? 0 : -1;
		HPRINT("SYSCALL meminfo(0x%lX) = %d\n", (long)dst, ret);
		if (ret == 0) {
			machine.copy_to_guest(dst, &result, sizeof(result));
		}
		machine.set_result(ret);
		machine.penalize(COMPLEX_CALL_PENALTY);
	});
}

template <int W>
const Arena& Machine<W>::arena() const {
	if (UNLIKELY(m_arena == nullptr))
		throw MachineException(SYSTEM_CALL_FAILED, "Arena not created on this machine");
	return *m_arena;
}
template <int W>
Arena& Machine<W>::arena() {
	if (UNLIKELY(m_arena == nullptr))
		throw MachineException(SYSTEM_CALL_FAILED, "Arena not created on this machine");
	return *m_arena;
}
template <int W>
void Machine<W>::setup_native_heap(size_t sysnum, uint64_t base, size_t max_memory)
{
	m_arena.reset(new Arena(base, base + max_memory));

	this->setup_native_heap_internal(sysnum);
}

template <int W>
void Machine<W>::setup_native_memory(const size_t syscall_base)
{
	this->install_syscall_handlers({
		{syscall_base+0, [] (Machine<W>& m) {
		// Memcpy n+0
		auto [dst, src, len] =
			m.sysargs<address_type<W>, address_type<W>, address_type<W>> ();
		MPRINT("SYSCALL memcpy(%#lX, %#lX, %zu)\n", (long)dst, (long)src, (size_t)len);
		m.memory.foreach(src, len,
			[dst = dst] (Memory<W>& m, address_type<W> off, const uint8_t* data, size_t len) {
				m.memcpy(dst + off, data, len);
			});
		m.penalize(2 * len);
	}}, {syscall_base+1, [] (Machine<W>& m) {
		// Memset n+1
		const auto [dst, value, len] =
			m.sysargs<address_type<W>, int, address_type<W>> ();
		MPRINT("SYSCALL memset(%#lX, %#X, %zu)\n", (long)dst, value, (size_t)len);
		m.memory.memset(dst, value, len);
		m.penalize(len);
	}}, {syscall_base+2, [] (Machine<W>& m) {
		// Memmove n+2
		auto [dst, src, len] =
			m.sysargs<address_type<W>, address_type<W>, address_type<W>> ();
		MPRINT("SYSCALL memmove(%#lX, %#lX, %zu)\n",
			(long) dst, (long) src, (size_t)len);
		// If the buffers don't overlap, we can use memcpy which copies forwards
		if (dst < src) {
			m.memory.foreach(src, len,
				[dst = dst] (Memory<W>& m, address_type<W> off, const uint8_t* data, size_t len) {
					m.memcpy(dst + off, data, len);
				});
		}
		else if (len > 0)
		{
			constexpr size_t wordsize = sizeof(address_type<W>);
			if (dst % wordsize == 0 && src % wordsize == 0 && len % wordsize == 0)
			{
				// Copy whole registers backwards
				// We start at len because unsigned doesn't have negative numbers
				// so we will have to read and write from index i-1 instead.
				for (unsigned i = len; i != 0; i -= wordsize) {
					m.memory.template write<address_type<W>> (dst + i-wordsize,
						m.memory.template read<address_type<W>> (src + i-wordsize));
				}
			} else {
				// Copy byte by byte backwards
				for (unsigned i = len; i != 0; i--) {
					m.memory.template write<uint8_t> (dst + i-1,
						m.memory.template read<uint8_t> (src + i-1));
				}
			}
		}
		m.penalize(2 * len);
	}}, {syscall_base+3, [] (Machine<W>& m) {
		// Memcmp n+3
		auto [p1, p2, len] =
			m.sysargs<address_type<W>, address_type<W>, address_type<W>> ();
		MPRINT("SYSCALL memcmp(%#lX, %#lX, %zu)\n", (long)p1, (long)p2, (size_t)len);
		m.penalize(2 * len);
		m.set_result(m.memory.memcmp(p1, p2, len));
	}}, {syscall_base+5, [] (Machine<W>& m) {
		// Strlen n+5
		auto [addr] = m.sysargs<address_type<W>> ();
		uint32_t len = m.memory.strlen(addr, STRLEN_MAX);
		m.penalize(2 * len);
		m.set_result(len);
		MPRINT("SYSCALL strlen(%#lX) = %u\n", (long)addr, len);
	}}, {syscall_base+6, [] (Machine<W>& m) {
		// Strncmp n+6
		auto [a1, a2, maxlen] =
			m.sysargs<address_type<W>, address_type<W>, uint32_t> ();
		MPRINT("SYSCALL strncmp(%#lX, %#lX, %u)\n", (long)a1, (long)a2, maxlen);
		uint32_t len = 0;
		while (len < maxlen) {
			const uint8_t v1 = m.memory.template read<uint8_t> (a1 ++);
			const uint8_t v2 = m.memory.template read<uint8_t> (a2 ++);
			if (v1 != v2 || v1 == 0) {
				m.penalize(2 + 2 * len);
				m.set_result(v1 - v2);
				return;
			}
			len ++;
		}
		m.penalize(2 + 2 * len);
		m.set_result(0);
	}}, {syscall_base+13, [] (Machine<W>& m) {
		// Describe value n+13
		auto [desc, value] =
			m.sysargs<std::string, address_type<W>> ();
		char buffer[256];
		const int len =
			snprintf(buffer, sizeof(buffer),
			"SYSCALL describe %s: 0x%lX (%ld)\n", desc.c_str(), (long)value, (long)value);
		m.debug_print(buffer, len);
	}}, {syscall_base+14, [] (Machine<W>& m) {
		// Print backtrace n+14
		m.memory.print_backtrace(
			[&] (std::string_view line) {
				m.print(line.begin(), line.size());
				m.print("\n", 1);
			});
		m.set_result(0);
		m.penalize(COMPLEX_CALL_PENALTY);
	}}});
}

template struct Machine<4>;
template struct Machine<8>;
} // riscv
