#include "machine.hpp"

#include "internal_common.hpp"
#include "decoded_exec_segment.hpp"
#include "decoder_cache.hpp"
#include "elf.hpp"
#include "native_heap.hpp"
#include "threaded_bytecodes.hpp"
#include <cstring>
#include <inttypes.h>

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
	static constexpr uint64_t MEMCPY_MAX = 1024ull * 1024u * 512u; // 512M
	static constexpr size_t   MEMCPY_BUFFERS = 256u; /* 1MB of maximally fragmented memory */
	static constexpr uint32_t STRLEN_MAX = 64'000u;
	static constexpr uint64_t COMPLEX_CALL_PENALTY = 2'000u;

/// @brief memmove() on guest memory, handling overlap and fragmented pages
template <int W>
static void native_memmove(Machine<W>& m,
	address_type<W> dst, address_type<W> src, address_type<W> len)
{
	if (UNLIKELY(len == 0))
		return;
	if (UNLIKELY(len > MEMCPY_MAX))
		throw MachineException(SYSTEM_CALL_FAILED, "memmove length too large", len);
	// If we have a flat readwrite arena, we can use memmove
	if constexpr (riscv::flat_readwrite_arena) {
		if (m.memory.try_memmove(dst, src, len)) {
			m.penalize(2 * len);
			return;
		}
	}
	// If the buffers don't overlap, we can use memcpy which copies forwards
	if (dst < src) {
		std::array<riscv::vBuffer, MEMCPY_BUFFERS> buffers;
		const size_t cnt =
			m.memory.gather_buffers_from_range(buffers.size(), buffers.data(), src, len);
		for (size_t i = 0; i < cnt; i++) {
			m.memory.memcpy(dst, buffers[i].ptr, buffers[i].len);
			dst += buffers[i].len;
		}
	}
	else
	{
		constexpr size_t wordsize = sizeof(address_type<W>);
		if (dst % wordsize == 0 && src % wordsize == 0 && len % wordsize == 0)
		{
			// Copy whole registers backwards
			// We start at len because unsigned doesn't have negative numbers
			// so we will have to read and write from index i-1 instead.
			for (address_type<W> i = len; i != 0; i -= wordsize) {
				m.memory.template write<address_type<W>> (dst + i-wordsize,
					m.memory.template read<address_type<W>> (src + i-wordsize));
			}
		} else {
			// Copy byte by byte backwards
			for (address_type<W> i = len; i != 0; i--) {
				m.memory.template write<uint8_t> (dst + i-1,
					m.memory.template read<uint8_t> (src + i-1));
			}
		}
	}
	m.penalize(2 * len);
}

template <int W>
void Machine<W>::setup_native_heap_internal(const size_t syscall_base)
{
	// Malloc n+0
	Machine<W>::install_syscall_handler(syscall_base+0,
	[] (Machine<W>& machine)
	{
		const size_t len = machine.sysarg(0);
		const address_t data = machine.arena().malloc(len);
		HPRINT("SYSCALL malloc(%zu) = 0x%lX\n", len, (long)data);
		machine.set_result(data);
		machine.penalize(COMPLEX_CALL_PENALTY);
	});
	// Calloc n+1
	Machine<W>::install_syscall_handler(syscall_base+1,
	[] (Machine<W>& machine)
	{
		const auto [count, size] =
			machine.template sysargs<address_type<W>, address_type<W>> ();
		// The multiplication must not be allowed to overflow
		const size_t len = size_t(count) * size_t(size);
		if (UNLIKELY(count != 0 && size != 0 && len / size_t(count) != size_t(size))) {
			machine.set_result(0);
			machine.penalize(COMPLEX_CALL_PENALTY);
			return;
		}
		const address_t data = machine.arena().malloc(len);
		HPRINT("SYSCALL calloc(%zu, %zu) = 0x%lX\n",
			(size_t)count, (size_t)size, (long)data);
		if (data != 0) {
			// XXX: Not using memzero as it has known issues
			machine.memory.memset(data, 0, len);
			machine.penalize(len);
		}
		machine.set_result(data);
		machine.penalize(COMPLEX_CALL_PENALTY);
	});
	// Realloc n+2
	Machine<W>::install_syscall_handler(syscall_base+2,
	[] (Machine<W>& machine)
	{
		const auto src = machine.sysarg(0);
		const auto newlen = machine.sysarg(1);

		const auto [data, srclen] = machine.arena().realloc(src, newlen);
		HPRINT("SYSCALL realloc(0x%lX:%zu, %zu) = 0x%lX\n",
			(long)src, (size_t)srclen, (size_t)newlen, (long)data);
		// When data != src, srclen is the old length, and the
		// chunks are non-overlapping, so we can use forwards memcpy.
		if (data != src && srclen != 0) {
			machine.memory.memcpy(data, machine, src, std::min(address_t(srclen), newlen));
			machine.penalize(2 * srclen);
		}
		machine.set_result((address_t)data);
		machine.penalize(COMPLEX_CALL_PENALTY);
	});
	// Free n+3
	Machine<W>::install_syscall_handler(syscall_base+3,
	[] (Machine<W>& machine)
	{
		const auto ptr = machine.sysarg(0);
		if (ptr != 0x0)
		{
			[[maybe_unused]] int ret = machine.arena().free(ptr);
			HPRINT("SYSCALL free(0x%lX) = %d\n", (long)ptr, ret);
			//machine.set_result(ret);
			if (ret < 0) {
				throw MachineException(SYSTEM_CALL_FAILED, "Possible double-free for freed pointer", ptr);
			}
			machine.penalize(COMPLEX_CALL_PENALTY);
			return;
		}
		HPRINT("SYSCALL free(0x0) = 0\n");
		//machine.set_result(0);
		machine.penalize(COMPLEX_CALL_PENALTY);
		return;
	});
	// Meminfo n+4
	Machine<W>::install_syscall_handler(syscall_base+4,
	[] (Machine<W>& machine)
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
void Machine<W>::transfer_arena_from(const Machine& other)
{
	m_arena.reset(new Arena(other.arena()));
}

template <int W>
void Machine<W>::setup_native_memory(const size_t syscall_base)
{
	Machine<W>::install_syscall_handlers({
		{syscall_base+0, [] (Machine<W>& m) {
		// Memcpy n+0
		auto [dst, src, len] =
			m.sysargs<address_type<W>, address_type<W>, address_type<W>> ();
		MPRINT("SYSCALL memcpy(%#lX, %#lX, %zu)\n", (long)dst, (long)src, (size_t)len);
		// A zero-length operation touches no memory, and the addresses do not
		// have to be valid: a Rust guest hands out a dangling pointer for
		// every empty slice, and libc is expected to leave it alone
		if (UNLIKELY(len == 0))
			return;
		if (UNLIKELY(len > MEMCPY_MAX))
			throw MachineException(SYSTEM_CALL_FAILED, "memcpy length too large", len);
		m.memory.memcpy(dst, m, src, len);
		m.penalize(2 * len);
	}}, {syscall_base+1, [] (Machine<W>& m) {
		// Memset n+1
		const auto [dst, value, len] =
			m.sysargs<address_type<W>, int, address_type<W>> ();
		MPRINT("SYSCALL memset(%#lX, %#X, %zu)\n", (long)dst, value, (size_t)len);
		if (UNLIKELY(len > MEMCPY_MAX))
			throw MachineException(SYSTEM_CALL_FAILED, "memset length too large", len);
		if (UNLIKELY(len == 0))
			return;
		m.memory.memset(dst, value, len);
		m.penalize(len);
	}}, {syscall_base+2, [] (Machine<W>& m) {
		// Memmove n+2
		auto [dst, src, len] =
			m.sysargs<address_type<W>, address_type<W>, address_type<W>> ();
		MPRINT("SYSCALL memmove(%#lX, %#lX, %zu)\n",
			(long) dst, (long) src, (size_t)len);
		native_memmove<W>(m, dst, src, len);
	}}, {syscall_base+3, [] (Machine<W>& m) {
		// Memcmp n+3
		auto [p1, p2, len] =
			m.sysargs<address_type<W>, address_type<W>, address_type<W>> ();
		MPRINT("SYSCALL memcmp(%#lX, %#lX, %zu)\n", (long)p1, (long)p2, (size_t)len);
		if (UNLIKELY(len > MEMCPY_MAX))
			throw MachineException(SYSTEM_CALL_FAILED, "memcmp length too large", len);
		if (UNLIKELY(len == 0)) {
			m.set_result(0);
			return;
		}
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
		maxlen = std::min(maxlen, STRLEN_MAX);
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
		// Reserved system call n+13
		// Space for one more accelerated libc function
		m.set_result(-1);
	}}, {syscall_base+14, [] (Machine<W>& m) {
		// Print backtrace n+14
		m.memory.print_backtrace(
			[&] (std::string_view line) {
				m.print(line.data(), line.size());
				m.print("\n", 1);
			});
		m.set_result(0);
		m.penalize(100 * COMPLEX_CALL_PENALTY);
	}}});
}

/**
 * Native libc fast-path (hot-patching)
**/
#ifndef LIBRISCV_LIBC_FASTPATH_STATS
#define LIBRISCV_LIBC_FASTPATH_STATS 0
#endif

namespace {
	// EBREAK is 0x00100073. Bits 7-11 (rd) are unused by EBREAK and carry the id.
	static constexpr uint32_t FASTPATH_EBREAK  = 0x00100073;
	static constexpr uint32_t FASTPATH_ID_MASK = 0x1Fu << 7;

	enum LibcFn : uint8_t {
		FN_INVALID = 0,
		FN_MEMCPY, FN_MEMSET, FN_MEMMOVE, FN_MEMCMP, FN_MEMCHR, FN_RAWMEMCHR,
		FN_STRLEN, FN_STRNLEN, FN_STRCMP, FN_STRNCMP,
		FN_STRCPY, FN_STPCPY, FN_STRNCPY, FN_STRCHR, FN_STRCHRNUL,
		FN_MAX
	};
	static_assert(FN_MAX <= 32, "The function id must fit in the 5-bit rd field");

	struct LibcSymbol {
		const char* name;
		uint8_t     id;
	};
	// Symbol names to look for. Aliases that share an implementation (bcmp,
	// __memcmpeq) are fine: they are all valid uses of the same routine.
	static constexpr LibcSymbol libc_symbols[] {
		{"memcpy",       FN_MEMCPY},    {"__memcpy",     FN_MEMCPY},
		{"__GI_memcpy",  FN_MEMCPY},
		{"memset",       FN_MEMSET},    {"__memset",     FN_MEMSET},
		{"__GI_memset",  FN_MEMSET},
		{"memmove",      FN_MEMMOVE},   {"__memmove",    FN_MEMMOVE},
		{"__GI_memmove", FN_MEMMOVE},
		{"memcmp",       FN_MEMCMP},    {"__memcmp",     FN_MEMCMP},
		{"__GI_memcmp",  FN_MEMCMP},    {"bcmp",         FN_MEMCMP},
		{"__memcmpeq",   FN_MEMCMP},
		{"memchr",       FN_MEMCHR},    {"__memchr",     FN_MEMCHR},
		{"__GI_memchr",  FN_MEMCHR},
		{"rawmemchr",    FN_RAWMEMCHR}, {"__rawmemchr",  FN_RAWMEMCHR},
		{"strlen",       FN_STRLEN},    {"__strlen",     FN_STRLEN},
		{"__GI_strlen",  FN_STRLEN},
		{"strnlen",      FN_STRNLEN},   {"__strnlen",    FN_STRNLEN},
		{"strcmp",       FN_STRCMP},    {"__strcmp",     FN_STRCMP},
		{"__GI_strcmp",  FN_STRCMP},
		{"strncmp",      FN_STRNCMP},   {"__strncmp",    FN_STRNCMP},
		{"strcpy",       FN_STRCPY},    {"__strcpy",     FN_STRCPY},
		{"stpcpy",       FN_STPCPY},    {"__stpcpy",     FN_STPCPY},
		{"strncpy",      FN_STRNCPY},   {"__strncpy",    FN_STRNCPY},
		{"strchr",       FN_STRCHR},    {"__strchr",     FN_STRCHR},
		{"index",        FN_STRCHR},
		{"strchrnul",    FN_STRCHRNUL}, {"__strchrnul",  FN_STRCHRNUL},
	};

#if LIBRISCV_LIBC_FASTPATH_STATS
	struct FastpathStats {
		uint64_t calls[FN_MAX] {};
		uint64_t bytes[FN_MAX] {};
		~FastpathStats() {
			static const char* names[FN_MAX] = {
				"(invalid)", "memcpy", "memset", "memmove", "memcmp", "memchr",
				"rawmemchr", "strlen", "strnlen", "strcmp", "strncmp",
				"strcpy", "stpcpy", "strncpy", "strchr", "strchrnul"
			};
			uint64_t total_calls = 0, total_bytes = 0;
			fprintf(stderr, "--- libc fast-path statistics ---\n");
			for (unsigned i = 1; i < FN_MAX; i++) {
				if (calls[i] == 0) continue;
				fprintf(stderr, "%-12s %12lu calls  %14lu bytes  (avg %.1f)\n",
					names[i], (unsigned long)calls[i], (unsigned long)bytes[i],
					double(bytes[i]) / double(calls[i]));
				total_calls += calls[i];
				total_bytes += bytes[i];
			}
			fprintf(stderr, "%-12s %12lu calls  %14lu bytes\n", "TOTAL",
				(unsigned long)total_calls, (unsigned long)total_bytes);
		}
	};
	static FastpathStats fastpath_stats;
	#define FASTPATH_COUNT(id, len) \
		do { fastpath_stats.calls[id]++; fastpath_stats.bytes[id] += (len); } while (0)
#else
	#define FASTPATH_COUNT(id, len) /* */
#endif
} // namespace

/// @brief Produce a host-contiguous view of at most one page starting at addr.
/// @details Guest memory is only guaranteed to be contiguous within a page when
/// virtual paging is enabled, so every scanning helper below advances one page
/// at a time. With a flat arena memview() is a bounds-check and a pointer.
template <int W>
static inline std::string_view page_view(const Machine<W>& m, address_type<W> addr, uint64_t maxlen)
{
	const uint64_t page_off = addr & (riscv::PageSize - 1);
	const uint64_t chunk = std::min(maxlen, uint64_t(riscv::PageSize) - page_off);
	// memview() either covers the whole request or throws a page fault, the
	// same way the original instructions would have faulted
	const auto view = m.memory.memview(addr, chunk, chunk);
	if (UNLIKELY(view.size() != chunk))
		throw MachineException(SYSTEM_CALL_FAILED, "libc fast-path got a short memory view", addr);
	return view;
}

/// @brief An unterminated string must stop *somewhere*, but unlike the opt-in
/// system calls above a hot-patched function is invisible to the guest, so a
/// truncated answer would be a silent wrong answer. Scanning is bounded by the
/// same limit as the bulk operations and reaching it is an error. In practice a
/// runaway scan hits unmapped memory long before this and faults there, exactly
/// as it would on real hardware.
static constexpr uint64_t FASTPATH_SCAN_MAX = MEMCPY_MAX;

[[noreturn]] static void fastpath_scan_overrun(uint64_t addr)
{
	throw MachineException(SYSTEM_CALL_FAILED,
		"libc fast-path scanned past the maximum string length", addr);
}

/// @brief strnlen() on guest memory
template <int W>
static uint64_t guest_strnlen(const Machine<W>& m, address_type<W> addr, uint64_t maxlen)
{
	uint64_t len = 0;
	while (len < maxlen) {
		const auto view = page_view<W>(m, addr + len, maxlen - len);
		const size_t n = ::strnlen(view.data(), view.size());
		len += n;
		if (n != view.size())
			return len;
	}
	return maxlen;
}

/// @brief memchr() on guest memory. Returns the offset, or -1 when not found.
template <int W>
static int64_t guest_memchr(const Machine<W>& m, address_type<W> addr, uint8_t value, uint64_t len)
{
	uint64_t offset = 0;
	while (offset < len) {
		const auto view = page_view<W>(m, addr + offset, len - offset);
		if (const void* hit = std::memchr(view.data(), value, view.size()); hit != nullptr)
			return int64_t(offset + (static_cast<const char*>(hit) - view.data()));
		offset += view.size();
	}
	return -1;
}

/// @brief strchrnul() on guest memory: the offset of the first occurrence of
/// value, or of the terminating NUL when there is none. found is set to false
/// when the scan stopped on the NUL instead of on value.
template <int W>
static uint64_t guest_strchrnul(const Machine<W>& m, address_type<W> addr,
	uint8_t value, uint64_t maxlen, bool& found)
{
	uint64_t offset = 0;
	while (offset < maxlen) {
		const auto view = page_view<W>(m, addr + offset, maxlen - offset);
		const char* hit = (const char*)std::memchr(view.data(), value, view.size());
		const char* nul = (const char*)std::memchr(view.data(), 0, view.size());
		if (hit != nullptr && (nul == nullptr || hit <= nul)) {
			found = true;
			return offset + (hit - view.data());
		}
		if (nul != nullptr) {
			found = false;
			return offset + (nul - view.data());
		}
		offset += view.size();
	}
	found = false;
	return maxlen;
}

/// @brief strncmp() on guest memory, stopping at the first NUL.
/// scanned receives the number of bytes that were examined.
template <int W>
static int guest_strncmp(const Machine<W>& m, address_type<W> a1, address_type<W> a2,
	uint64_t maxlen, uint64_t& scanned)
{
	uint64_t offset = 0;
	while (offset < maxlen) {
		// Both strings must stay inside their own page for this round
		const uint64_t rem1 = riscv::PageSize - ((a1 + offset) & (riscv::PageSize - 1));
		const uint64_t rem2 = riscv::PageSize - ((a2 + offset) & (riscv::PageSize - 1));
		const uint64_t chunk = std::min(maxlen - offset, std::min(rem1, rem2));
		const auto v1 = page_view<W>(m, a1 + offset, chunk);
		const auto v2 = page_view<W>(m, a2 + offset, chunk);

		// Compare up to and including the NUL of the first string, if present
		const size_t zero = ::strnlen(v1.data(), chunk);
		const size_t cmplen = std::min(zero + 1, size_t(chunk));
		if (std::memcmp(v1.data(), v2.data(), cmplen) != 0) {
			// Return the exact difference, the way glibc does
			size_t i = 0;
			while (v1[i] == v2[i]) i++;
			scanned = offset + i + 1;
			return int(uint8_t(v1[i])) - int(uint8_t(v2[i]));
		}
		if (zero < chunk) {
			scanned = offset + zero + 1;
			return 0; // Both strings ended here
		}
		offset += chunk;
	}
	scanned = offset;
	return 0;
}

/// @brief The native implementation of one hot-patched libc function.
/// @details Arguments are read straight out of a0-a2 and the result is written
/// back to a0, exactly like the RISC-V calling convention prescribes. The
/// caller-saved registers a real libc routine would clobber are left untouched,
/// which no conforming caller can observe.
template <int W>
static void run_libc_fastpath(Machine<W>& m, unsigned id)
{
	using address_t = address_type<W>;
	auto& cpu = m.cpu;
	const address_t arg0 = cpu.reg(REG_ARG0 + 0);
	const address_t arg1 = cpu.reg(REG_ARG0 + 1);
	const address_t arg2 = cpu.reg(REG_ARG0 + 2);

	switch (id) {
	case FN_MEMCPY: {
		MPRINT("FASTPATH memcpy(%#lX, %#lX, %zu)\n", (long)arg0, (long)arg1, (size_t)arg2);
		FASTPATH_COUNT(FN_MEMCPY, arg2);
		if (LIKELY(arg2 != 0)) {
			if (UNLIKELY(arg2 > MEMCPY_MAX))
				throw MachineException(SYSTEM_CALL_FAILED, "memcpy length too large", arg2);
			m.memory.memcpy(arg0, m, arg1, arg2);
			m.penalize(2 * arg2);
		}
		// memcpy returns dst, which is already in a0
		return;
	}
	case FN_MEMSET: {
		MPRINT("FASTPATH memset(%#lX, %#X, %zu)\n", (long)arg0, (int)arg1, (size_t)arg2);
		FASTPATH_COUNT(FN_MEMSET, arg2);
		if (LIKELY(arg2 != 0)) {
			if (UNLIKELY(arg2 > MEMCPY_MAX))
				throw MachineException(SYSTEM_CALL_FAILED, "memset length too large", arg2);
			m.memory.memset(arg0, uint8_t(arg1), arg2);
			m.penalize(arg2);
		}
		// memset returns dst
		return;
	}
	case FN_MEMMOVE: {
		MPRINT("FASTPATH memmove(%#lX, %#lX, %zu)\n", (long)arg0, (long)arg1, (size_t)arg2);
		FASTPATH_COUNT(FN_MEMMOVE, arg2);
		// Shares the implementation with the accelerated memmove system call
		native_memmove<W>(m, arg0, arg1, arg2);
		// memmove returns dst, which is already in a0
		return;
	}
	case FN_MEMCMP: {
		MPRINT("FASTPATH memcmp(%#lX, %#lX, %zu)\n", (long)arg0, (long)arg1, (size_t)arg2);
		FASTPATH_COUNT(FN_MEMCMP, arg2);
		if (UNLIKELY(arg2 > MEMCPY_MAX))
			throw MachineException(SYSTEM_CALL_FAILED, "memcmp length too large", arg2);
		if (UNLIKELY(arg2 == 0)) {
			m.set_result(0);
			return;
		}
		m.penalize(2 * arg2);
		m.set_result(m.memory.memcmp(arg0, arg1, arg2));
		return;
	}
	case FN_MEMCHR: {
		MPRINT("FASTPATH memchr(%#lX, %#X, %zu)\n", (long)arg0, (int)arg1, (size_t)arg2);
		FASTPATH_COUNT(FN_MEMCHR, arg2);
		if (UNLIKELY(arg2 > MEMCPY_MAX))
			throw MachineException(SYSTEM_CALL_FAILED, "memchr length too large", arg2);
		const int64_t off = (arg2 != 0) ? guest_memchr<W>(m, arg0, uint8_t(arg1), arg2) : -1;
		m.penalize(2 * ((off < 0) ? arg2 : uint64_t(off)));
		m.set_result(address_t((off < 0) ? 0 : arg0 + off));
		return;
	}
	case FN_RAWMEMCHR: {
		MPRINT("FASTPATH rawmemchr(%#lX, %#X)\n", (long)arg0, (int)arg1);
		// rawmemchr() promises the byte is there, so scanning is only bounded
		// to keep a corrupt guest from scanning all of memory
		const int64_t off = guest_memchr<W>(m, arg0, uint8_t(arg1), MEMCPY_MAX);
		if (UNLIKELY(off < 0))
			throw MachineException(SYSTEM_CALL_FAILED, "rawmemchr found no match", arg0);
		FASTPATH_COUNT(FN_RAWMEMCHR, uint64_t(off));
		m.penalize(2 * uint64_t(off));
		m.set_result(address_t(arg0 + off));
		return;
	}
	case FN_STRLEN: {
		const uint64_t len = guest_strnlen<W>(m, arg0, FASTPATH_SCAN_MAX);
		if (UNLIKELY(len == FASTPATH_SCAN_MAX))
			fastpath_scan_overrun(arg0);
		MPRINT("FASTPATH strlen(%#lX) = %zu\n", (long)arg0, (size_t)len);
		FASTPATH_COUNT(FN_STRLEN, len);
		m.penalize(2 * len);
		m.set_result(address_t(len));
		return;
	}
	case FN_STRNLEN: {
		// A bound of SIZE_MAX is a normal way of saying "just find the NUL"
		const uint64_t len = guest_strnlen<W>(m, arg0, std::min<uint64_t>(arg1, FASTPATH_SCAN_MAX));
		if (UNLIKELY(len == FASTPATH_SCAN_MAX && arg1 > FASTPATH_SCAN_MAX))
			fastpath_scan_overrun(arg0);
		MPRINT("FASTPATH strnlen(%#lX, %zu) = %zu\n", (long)arg0, (size_t)arg1, (size_t)len);
		FASTPATH_COUNT(FN_STRNLEN, len);
		m.penalize(2 * len);
		m.set_result(address_t(len));
		return;
	}
	case FN_STRCMP: {
		uint64_t scanned = 0;
		const int result = guest_strncmp<W>(m, arg0, arg1, FASTPATH_SCAN_MAX, scanned);
		if (UNLIKELY(scanned == FASTPATH_SCAN_MAX))
			fastpath_scan_overrun(arg0);
		MPRINT("FASTPATH strcmp(%#lX, %#lX) = %d\n", (long)arg0, (long)arg1, result);
		FASTPATH_COUNT(FN_STRCMP, scanned);
		m.penalize(2 * scanned);
		m.set_result(result);
		return;
	}
	case FN_STRNCMP: {
		uint64_t scanned = 0;
		const int result = guest_strncmp<W>(m, arg0, arg1,
			std::min<uint64_t>(arg2, FASTPATH_SCAN_MAX), scanned);
		if (UNLIKELY(scanned == FASTPATH_SCAN_MAX && arg2 > FASTPATH_SCAN_MAX))
			fastpath_scan_overrun(arg0);
		MPRINT("FASTPATH strncmp(%#lX, %#lX, %zu) = %d\n",
			(long)arg0, (long)arg1, (size_t)arg2, result);
		FASTPATH_COUNT(FN_STRNCMP, scanned);
		m.penalize(2 * scanned);
		m.set_result(result);
		return;
	}
	case FN_STRCPY:
	case FN_STPCPY: {
		const uint64_t len = guest_strnlen<W>(m, arg1, FASTPATH_SCAN_MAX);
		if (UNLIKELY(len == FASTPATH_SCAN_MAX))
			fastpath_scan_overrun(arg1);
		MPRINT("FASTPATH strcpy(%#lX, %#lX) = %zu\n", (long)arg0, (long)arg1, (size_t)len);
		FASTPATH_COUNT(id, len);
		m.memory.memcpy(arg0, m, arg1, address_t(len + 1)); // Including the NUL
		m.penalize(2 * len);
		// strcpy() returns dst, stpcpy() returns the new end of dst
		m.set_result(address_t(arg0 + ((id == FN_STPCPY) ? len : 0)));
		return;
	}
	case FN_STRNCPY: {
		if (UNLIKELY(arg2 > MEMCPY_MAX))
			throw MachineException(SYSTEM_CALL_FAILED, "strncpy length too large", arg2);
		const uint64_t len = guest_strnlen<W>(m, arg1, arg2);
		MPRINT("FASTPATH strncpy(%#lX, %#lX, %zu) = %zu\n",
			(long)arg0, (long)arg1, (size_t)arg2, (size_t)len);
		FASTPATH_COUNT(FN_STRNCPY, arg2);
		if (len != 0)
			m.memory.memcpy(arg0, m, arg1, address_t(len));
		// strncpy() pads the remainder of the destination with NUL bytes
		if (len < arg2)
			m.memory.memset(arg0 + len, 0, arg2 - len);
		m.penalize(2 * arg2);
		// strncpy returns dst
		return;
	}
	case FN_STRCHR:
	case FN_STRCHRNUL: {
		bool found = false;
		const uint64_t off = guest_strchrnul<W>(m, arg0, uint8_t(arg1), FASTPATH_SCAN_MAX, found);
		if (UNLIKELY(off == FASTPATH_SCAN_MAX))
			fastpath_scan_overrun(arg0);
		MPRINT("FASTPATH strchr(%#lX, %#X) = %zu\n", (long)arg0, (int)arg1, (size_t)off);
		FASTPATH_COUNT(id, off);
		m.penalize(2 * off);
		// strchr() returns NULL when the character is absent, while
		// strchrnul() returns the address of the terminating NUL
		if (found || id == FN_STRCHRNUL)
			m.set_result(address_t(arg0 + off));
		else
			m.set_result(address_t(0));
		return;
	}
	default:
		throw MachineException(SYSTEM_CALL_FAILED, "Invalid libc fast-path function", id);
	}
}

/// @brief The EBREAK handler that all hot-patched libc functions land in.
template <int W>
static void libc_fastpath_ebreak_handler(Machine<W>& m)
{
	auto& exec = m.cpu.current_execute_segment();
	const auto pc = m.cpu.pc();

	// The patched entry is always the last instruction of its block, so the
	// dispatcher's PC is exact and can be used to find the entry back again.
	if (LIKELY(exec.is_within(pc, 4)))
	{
		const auto& entry = exec.decoder_cache()[pc / DecoderData<W>::DIVISOR];
		const uint32_t instr = entry.instr;
		if (LIKELY(entry.get_bytecode() == RV32I_BC_SYSTEM
			&& (instr & ~FASTPATH_ID_MASK) == FASTPATH_EBREAK))
		{
			const unsigned id = (instr & FASTPATH_ID_MASK) >> 7;
			if (LIKELY(id != FN_INVALID && id < FN_MAX))
			{
				run_libc_fastpath<W>(m, id);
				// Return to the caller. RV32I_BC_SYSTEM notices that the
				// handler moved PC and resumes there instead of at pc+4.
				m.cpu.registers().pc = m.cpu.reg(REG_RA);
				return;
			}
		}
	}

	// Not one of ours: this is a real breakpoint
	Machine<W>::previous_ebreak_handler(m);
}

template <int W>
void Machine<W>::install_libc_fastpath_handler()
{
	// Chain onto whatever EBREAK handler is installed (the Linux system call
	// layer installs one, and so do the CLI debugger and the GDB stub), so
	// that hot-patching and real breakpoints can coexist. From here on
	// install_syscall_handler() redirects EBREAK installs into the chain
	// instead of replacing us, which makes the set-up order irrelevant.
	if (Machine<W>::m_libc_fastpath_ebreak == nullptr) {
		Machine<W>::m_previous_ebreak_handler = Machine<W>::syscall_handlers[SYSCALL_EBREAK];
		Machine<W>::m_libc_fastpath_ebreak = libc_fastpath_ebreak_handler<W>;
		Machine<W>::syscall_handlers[SYSCALL_EBREAK] = libc_fastpath_ebreak_handler<W>;
	}
}

template <int W>
void Machine<W>::previous_ebreak_handler(Machine<W>& m)
{
	if (Machine<W>::m_previous_ebreak_handler != nullptr)
		Machine<W>::m_previous_ebreak_handler(m);
}

template <int W>
size_t Machine<W>::install_libc_fastpath(DecodedExecuteSegment<W>& exec, bool verbose)
{
	this->install_libc_fastpath_handler();

	// Resolve every known name in a single pass over the symbol table.
	// Machine::address_of() would re-scan all symbols once per name, which is
	// measurable on a large binary (node has ~130k symbols).
	std::vector<std::pair<address_t, uint8_t>> found;
	memory.for_each_symbol(
		[&] (const typename riscv::Elf<W>::Sym& sym, const char* name)
	{
		if (name == nullptr || sym.st_value == 0)
			return;
		if (riscv::Elf<W>::SymbolType(sym.st_info) != riscv::Elf<W>::STT_FUNC)
			return;
		for (const auto& entry : libc_symbols) {
			if (std::strcmp(name, entry.name) == 0) {
				found.push_back({address_t(sym.st_value), entry.id});
				return;
			}
		}
	});

	size_t patched = 0;
	for (const auto& [addr, id] : found)
	{
		if (!exec.is_within(addr, 4))
			continue;
		auto& existing = exec.decoder_cache()[addr / DecoderData<W>::DIVISOR];
		// Aliases (bcmp/memcmp/__memcmpeq) resolve to the same address
		if (existing.get_bytecode() == RV32I_BC_SYSTEM
			&& (existing.instr & ~FASTPATH_ID_MASK) == FASTPATH_EBREAK)
			continue;

#if __cpp_exceptions
		try {
#endif
			// Make the function entry the last instruction of its block, so
			// that the dispatcher's PC is exact both when the function is
			// called and when the preceding block falls through into it
			auto& entry = CPU<W>::create_block_ending_entry_at(exec, addr);
			entry.instr = FASTPATH_EBREAK | (uint32_t(id) << 7);
			entry.set_bytecode(RV32I_BC_SYSTEM);
			entry.set_invalid_handler();
			entry.idxend = 0;
		#ifdef RISCV_EXT_C
			entry.icount = 0;
		#endif
			patched ++;
			if (verbose) {
				printf("libriscv: libc fast-path installed at 0x%" PRIx64 " (id %u)\n",
					uint64_t(addr), unsigned(id));
			}
#if __cpp_exceptions
		} catch (const std::exception& e) {
			// A function entry that cannot be turned into a block ending is
			// left alone: the guest simply keeps running the original code
			if (verbose) {
				printf("libriscv: libc fast-path skipped 0x%" PRIx64 ": %s\n",
					uint64_t(addr), e.what());
			}
		}
#endif
	}

	if (verbose) {
		printf("libriscv: libc fast-path patched %zu function(s)\n", patched);
	}
	return patched;
}

INSTANTIATE_32_IF_ENABLED(Machine);
INSTANTIATE_64_IF_ENABLED(Machine);
} // riscv
