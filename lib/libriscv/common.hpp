#pragma once
#include <type_traits>
#include <string>
#include "util/function.hpp"
#include "types.hpp"

#ifndef RISCV_SYSCALLS_MAX
#define RISCV_SYSCALLS_MAX   512
#endif

#ifndef RISCV_SYSCALL_EBREAK_NR
#define RISCV_SYSCALL_EBREAK_NR    (RISCV_SYSCALLS_MAX-1)
#endif

#ifndef RISCV_PAGE_SIZE
#define RISCV_PAGE_SIZE  4096UL
#endif

namespace riscv
{
	static constexpr int SYSCALL_EBREAK = RISCV_SYSCALL_EBREAK_NR;

	static constexpr size_t PageSize = RISCV_PAGE_SIZE;
	static constexpr size_t PageMask = RISCV_PAGE_SIZE-1;

#ifdef RISCV_MEMORY_TRAPS
	static constexpr bool memory_traps_enabled = true;
#else
	static constexpr bool memory_traps_enabled = false;
#endif

#ifdef RISCV_DEBUG
	static constexpr bool memory_alignment_check = true;
	static constexpr bool verbose_branches_enabled = true;
	static constexpr bool unaligned_memory_slowpaths = true;
#else
	static constexpr bool memory_alignment_check = false;
	static constexpr bool verbose_branches_enabled = false;
	static constexpr bool unaligned_memory_slowpaths = false;
#endif

#ifdef RISCV_EXT_ATOMICS
	static constexpr bool atomics_enabled = true;
#else
	static constexpr bool atomics_enabled = false;
#endif
#ifdef RISCV_EXT_COMPRESSED
	static constexpr bool compressed_enabled = true;
#else
	static constexpr bool compressed_enabled = false;
#endif
#ifdef RISCV_EXT_VECTOR
	static constexpr unsigned vector_extension = RISCV_EXT_VECTOR;
#else
	static constexpr unsigned vector_extension = 0;
#endif
#ifdef RISCV_BINARY_TRANSLATION
	static constexpr bool binary_translation_enabled = true;
#else
	static constexpr bool binary_translation_enabled = false;
#endif
#ifdef RISCV_THREADED
	static constexpr bool threaded_simulator_enabled = true;
#else
	static constexpr bool threaded_simulator_enabled = false;
#endif
}

namespace riscv
{
	template <int W> struct Memory;

	template <int W>
	struct MachineOptions
	{
		uint64_t memory_max = 16ull << 20; // 16MB
		uint64_t stack_size = 1ul << 20; // 1MB default stack
		unsigned cpu_id = 0;
		bool load_program = true;
		bool protect_segments = true;
		bool allow_write_exec_segment = false;
		bool enforce_exec_only = false;
		bool verbose_loader = false;
		// Minimal fork does not loan any pages from the source Machine
		bool minimal_fork = false;
		// Allow the use of a linear arena to increase memory locality somewhat
		bool use_memory_arena = true;

		riscv::Function<struct Page&(Memory<W>&, address_type<W>, bool)> page_fault_handler = nullptr;

#ifdef RISCV_BINARY_TRANSLATION
		unsigned block_size_treshold = 6;
		unsigned translate_blocks_max = 5000;
		unsigned translate_instr_max = 128'000;
#endif
	};

	template <int W> struct MultiThreading;
	template <int W> struct Multiprocessing;
	template <int W> struct SerializedMachine;
	struct Arena;

	template <class...> constexpr std::false_type always_false {};

	template<typename T>
	struct is_string
		: public std::disjunction<
			std::is_same<char *, typename std::decay<T>::type>,
			std::is_same<const char *, typename std::decay<T>::type>
	> {};

	template<class T>
	struct is_stdstring : public std::is_same<T, std::basic_string<char>> {};
} // riscv

#ifndef LIKELY
#define LIKELY(x) __builtin_expect((x), 1)
#endif
#ifndef UNLIKELY
#define UNLIKELY(x) __builtin_expect((x), 0)
#endif

#ifndef COLD_PATH
#define COLD_PATH() __attribute__((cold))
#endif

#ifndef RISCV_INTERNAL
#define RISCV_INTERNAL __attribute__((visibility("internal")))
#endif
