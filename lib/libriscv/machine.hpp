#pragma once
#include "common.hpp"
#include "cpu.hpp"
#include "memory.hpp"
#include "util/delegate.hpp"
#include <array>
#include <errno.h> // ENOSYS
#include <vector>

namespace riscv
{
	static constexpr int RISCV32 = 4;
	static constexpr int RISCV64 = 8;
	static constexpr uint64_t DEFAULT_MEMORY_MAX = 16ull << 20; // 16mb

	template <int W>
	struct Machine
	{
		using address_t = address_type<W>;          // one unsigned memory address
		using syscall_t = delegate<long (Machine<W>&)>;
		Machine(const std::vector<uint8_t>& binary = {},
				address_t max_memory = DEFAULT_MEMORY_MAX);
		~Machine();

		// Simulate a RISC-V machine until @max_instructions have been
		// executed, or the machine has been stopped.
		// NOTE: if @max_instructions is 0, then run until stop
		void simulate(uint64_t max_instructions = 0);

		void stop(bool v = true) noexcept;
		bool stopped() const noexcept;
		void reset();

		CPU<W>    cpu;
		Memory<W> memory;

		// Copy data into the guests memory
		address_t copy_to_guest(address_t dst, const void* buf, size_t length);
		// Push something onto the stack, and move the stack pointer
		address_t stack_push(const void* data, size_t length);
		address_t stack_push(const std::string& string);
		template <typename T>
		address_t stack_push(const T& pod_type);

		// Install a system call handler for a the given syscall number.
		// Pass nullptr to uninstall a system call handler.
		void install_syscall_handler(int, syscall_t);
		void install_syscall_handlers(std::initializer_list<std::pair<int, syscall_t>>);
		syscall_t get_syscall_handler(int);

		// Push all strings on stack and then create a mini-argv on SP
		void setup_argv(const std::vector<std::string>& args);

		// Retrieve arguments during a system call
		template <typename T>
		inline T sysarg(int arg) const;

		// Retrieve all arguments by given types during a system call
		template <typename... Args>
		inline auto sysargs() const;

		// Calls into the virtual machine, returning the value returned from
		// @function_name, which must be visible in the ELF symbol tables.
		// the function must use the C ABI calling convention.
		// The value of machine.stopped() should be false if the machine
		// reached max instructions without completing the function call.
		// Supports integers, floating-point values and strings.
		// Passing 0 to max instructions will disable the limit, and potentially
		// run forever.
		// NOTE: relies on an exit function to stop execution after returning.
		// _exit must call the exit (93) system call and not call destructors,
		// which is the norm.
		template<uint64_t MAXI = 0, typename... Args> constexpr
		address_t vmcall(const char* cfunction, Args&&... args);

		template<uint64_t MAXI = 0, typename... Args> constexpr
		address_t vmcall(address_t call_addr, Args&&... args);

		// Sets up a function call only, executes no instructions.
		// Supports integers, floating-point values and strings.
		// Strings will be put on stack, which is not restored automatically.
		template<typename... Args> constexpr
		void setup_call(address_t call_addr, Args&&... args);

		// returns the address of a symbol in the ELF symtab, or zero
		address_t address_of(const char* name);

		// Bytes (in whole pages) of unused memory
		address_t free_memory() const noexcept;

		// Call a function when the machine gets destroyed
		void add_destructor_callback(delegate<void()> callback);

#ifdef RISCV_DEBUG
		// Immediately block execution, print registers and current instruction.
		void print_and_pause();
		bool verbose_instructions = false;
		bool verbose_jumps     = false;
		bool verbose_registers = false;
		bool verbose_fp_registers = false;
#else
		static constexpr bool verbose_instructions = false;
		static constexpr bool verbose_jumps     = false;
		static constexpr bool verbose_registers = false;
#endif
		bool throw_on_unhandled_syscall = false;
		void system_call(int);

		// Realign the stack pointer, to make sure that function calls succeed
		void realign_stack();

		// Serializes all the machine state + a tiny header to @vec
		void serialize_to(std::vector<uint8_t>& vec);
		// Returns the machine to a previously stored state
		// NOTE: All previous memory traps are lost, syscall handlers,
		// destructor callbacks are kept. Page fault handler and
		// symbol lookup cache is also kept. Returns 0 on success.
		int deserialize_from(const std::vector<uint8_t>&);

	private:
		template<typename... Args, std::size_t... indices>
		auto resolve_args(std::index_sequence<indices...>) const;
		bool m_stopped = false;
		std::array<syscall_t, 512> m_syscall_handlers;
		std::vector<delegate<void()>> m_destructor_callbacks;
		static_assert((W == 4 || W == 8), "Must be either 4-byte or 8-byte ISA");
	};

#include "machine_inline.hpp"
}
