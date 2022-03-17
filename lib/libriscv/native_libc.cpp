#include "machine.hpp"
#include "native_heap.hpp"

//#define HPRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
#define HPRINT(fmt, ...) /* */
//#define MPRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
#define MPRINT(fmt, ...) /* */

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
		HPRINT("SYSCALL malloc(%zu) = 0x%X\n", len, data);
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
		HPRINT("SYSCALL calloc(%u, %u) = 0x%X\n", count, size, data);
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
		const address_type<W> src = machine.template sysarg<address_type<W>>(0);
		const size_t newlen = machine.template sysarg<address_type<W>>(1);
		if (src != 0)
		{
			const size_t srclen = machine.arena().size(src, false);
			if (srclen > 0)
			{
				if (srclen >= newlen) {
					return;
				}
				// XXX: We really have to know what we are doing here, as
				// we are freeing in the hopes of getting the same chunk
				// back, in which case the copy could be completely skipped.
				machine.arena().free(src);
				auto data = machine.arena().malloc(newlen);
				HPRINT("SYSCALL realloc(0x%X:%zu, %zu) = 0x%X\n", src, srclen, newlen, data);
				// If the reallocation fails, return NULL
				if (data == 0) {
					machine.arena().malloc(srclen);
					machine.set_result(data);
					machine.penalize(COMPLEX_CALL_PENALTY);
					return;
				}
				else if (data != src)
				{
					// XXX: Should this be a memmove?
					machine.memory.memcpy(data, machine, src, srclen);
				}
				machine.set_result(data);
				machine.penalize(COMPLEX_CALL_PENALTY);
				return;
			} else {
				HPRINT("SYSCALL realloc(0x%X:??, %zu) = 0x0\n", src, newlen);
			}
		} else {
			auto data = machine.arena().malloc(newlen);
			HPRINT("SYSCALL realloc(0x0, %zu) = 0x%lX\n", newlen, (long) data);
			machine.set_result(data);
			machine.penalize(COMPLEX_CALL_PENALTY);
			return;
		}
		machine.set_result(0);
		machine.penalize(COMPLEX_CALL_PENALTY);
	});
	// Free n+3
	this->install_syscall_handler(syscall_base+3,
	[] (auto& machine)
	{
		const auto ptr = machine.template sysarg<address_type<W>>(0);
		if (ptr != 0)
		{
			int ret = machine.arena().free(ptr);
			HPRINT("SYSCALL free(0x%X) = %d\n", ptr, ret);
			machine.set_result(ret);
			if (ptr != 0x0 && ret < 0) {
				throw MachineException(SYSTEM_CALL_FAILED, "Possible double-free for freed pointer");
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
		const auto dst = machine.template sysarg<address_type<W>>(0);
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
		HPRINT("SYSCALL meminfo(0x%X) = %d\n", dst, ret);
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
		MPRINT("SYSCALL memcpy(%#X, %#X, %u)\n", dst, src, len);
		m.memory.foreach(src, len,
			[dst = dst] (Memory<W>& m, address_type<W> off, const uint8_t* data, size_t len) {
				m.memcpy(dst + off, data, len);
			});
		m.penalize(2 * len);
	}}, {syscall_base+1, [] (Machine<W>& m) {
		// Memset n+1
		const auto [dst, value, len] =
			m.sysargs<address_type<W>, address_type<W>, address_type<W>> ();
		MPRINT("SYSCALL memset(%#X, %#X, %u)\n", dst, value, len);
		m.memory.memset(dst, value, len);
		m.penalize(len);
	}}, {syscall_base+2, [] (Machine<W>& m) {
		// Memmove n+2
		auto [dst, src, len] =
			m.sysargs<address_type<W>, address_type<W>, address_type<W>> ();
		MPRINT("SYSCALL memmove(%#lX, %#lX, %lu)\n",
			(long) dst, (long) src, (long) len);
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
		MPRINT("SYSCALL memcmp(%#X, %#X, %u)\n", p1, p2, len);
		m.penalize(2 * len);
		m.set_result(m.memory.memcmp(p1, p2, len));
	}}, {syscall_base+5, [] (Machine<W>& m) {
		// Strlen n+5
		auto [addr] = m.sysargs<address_type<W>> ();
		uint32_t len = m.memory.strlen(addr, STRLEN_MAX);
		m.penalize(2 * len);
		m.set_result(len);
		MPRINT("SYSCALL strlen(%#lX) = %lu\n",
			(long) addr, (long) len);
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
