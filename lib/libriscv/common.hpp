#pragma once
#include <type_traits>
#include <functional>
#include <string>

#ifndef RISCV_SYSCALLS_MAX
#define RISCV_SYSCALLS_MAX   512
#endif

#ifndef RISCV_SYSCALL_EBREAK_NR
#define RISCV_SYSCALL_EBREAK_NR    (RISCV_SYSCALLS_MAX-1)
#endif

#ifndef RISCV_PAGE_SIZE
#define RISCV_PAGE_SIZE  4096
#endif

#ifndef RISCV_RODATA_SEGMENT_IS_SHARED
#define RISCV_RODATA_SEGMENT_IS_SHARED 1
#endif

namespace riscv
{
	static constexpr int SYSCALL_EBREAK = RISCV_SYSCALL_EBREAK_NR;

	static constexpr int PageSize = RISCV_PAGE_SIZE;

#ifdef RISCV_MEMORY_TRAPS
	static constexpr bool memory_traps_enabled = true;
#else
	static constexpr bool memory_traps_enabled = false;
#endif

#ifdef RISCV_DEBUG
	static constexpr bool debugging_enabled = true;
	static constexpr bool memory_alignment_check = true;
#else
	static constexpr bool debugging_enabled = false;
	static constexpr bool memory_alignment_check = false;
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
#ifdef RISCV_EXT_FLOATS
	static constexpr bool floating_point_enabled = true;
#else
	static constexpr bool floating_point_enabled = false;
#endif
#ifdef RISCV_BINARY_TRANSLATION
	static constexpr bool binary_translation_enabled = true;
#else
	static constexpr bool binary_translation_enabled = false;
#endif
#ifdef RISCV_DECODER_REWRITER
	static constexpr bool decoder_rewriter_enabled = true;
#else
	static constexpr bool decoder_rewriter_enabled = false;
#endif
#ifdef RISCV_FAST_SIMULATOR
	static constexpr bool fast_simulator_enabled = true;
#else
	static constexpr bool fast_simulator_enabled = false;
#endif
}

namespace riscv
{
	template <int W> struct Memory;

	template <int W>
	struct MachineOptions
	{
		uint64_t memory_max = 16ull << 20; // 16mb
		unsigned cpu_id = 0;
		bool load_program = true;
		bool protect_segments = true;
		bool allow_write_exec_segment = false;
		bool enforce_exec_only = false;
		bool verbose_loader = false;
		// Minimal fork does not loan any pages from the source Machine
		bool minimal_fork = false;
		// Instruction fusing is an experimental optimizing feature
		// Can only be enabled with the RISCV_EXPERIMENTAL CMake option
		bool instruction_fusing = false;

		std::function<struct Page&(Memory<W>&, size_t, bool)> page_fault_handler = nullptr;

#ifdef RISCV_BINARY_TRANSLATION
		unsigned block_size_treshold = 8;
		unsigned translate_blocks_max = 4000;
		unsigned translate_instr_max = 128'000;
		bool forward_jumps = false;
#endif
	};

	template <int W> struct MultiThreading;
	template <int W> struct Multiprocessing;
	template <int W> struct SerializedMachine;
	template <int W> struct QCVec;
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
