#pragma once
#include "common.hpp"
#include "cpu.hpp"
#include "memory.hpp"
#include "util/delegate.hpp"
#include <map>
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
		Machine(const std::vector<uint8_t>& binary,
				address_t max_memory = DEFAULT_MEMORY_MAX);

		// Simulate a RISC-V machine until @max_instructions have been
		// executed, or the machine has been stopped.
		// NOTE: if @max_instructions is 0, then run until stop
		void simulate(uint64_t max_instructions = 0);

		void stop() noexcept;
		bool stopped() const noexcept;
		void install_syscall_handler(int, syscall_t);
		syscall_t get_syscall_handler(int);
		void reset();

		CPU<W>    cpu;
		Memory<W> memory;

		// Copy data into the guests memory
		address_t copy_to_guest(address_t dst, const void* buf, size_t length);
		// Push something onto the stack, and move the stack pointer
		address_t stack_push(const void* data, size_t length);

		// Push all strings on stack and then create a mini-argv on SP
		void setup_argv(const std::vector<std::string>& args);

		// Retrieve arguments during a system call
		template <typename T>
		inline T sysarg(int arg) const;

		// Calls into the virtual machine, returning the value returned from
		// @function_name, which must be visible in the ELF symbol tables.
		// the function must use the C ABI calling convention.
		// If max instructions are reached, the call returns -1, and reading
		// the instruction counter will show that the count was reached.
		// NOTE: overwrites the exit (93) system call and relies on _exit
		// to stop execution right after returning. _exit must call the exit
		// (93) system call and not call destructors, which is the norm.
		long vmcall(const std::string& function_name,
					std::initializer_list<address_t> args,
					uint64_t max_instructions = 0);

		// Sets up a function call only, executes no instructions.
		void setup_call(address_t call_addr, address_t retn_addr,
						std::initializer_list<address_t> args);

		// returns the address of a symbol in the ELF symtab, or zero
		address_t address_of(const std::string& name);

		// Realign the stack pointer, to make sure that vmcalls succeed
		void realign_stack(uint8_t align = 16);

#ifdef RISCV_DEBUG
		// Immediately block execution, print registers and current instruction.
		void print_and_pause();
		bool verbose_instructions = false;
		bool verbose_jumps     = false;
		bool verbose_registers = false;
#else
		static constexpr bool verbose_instructions = false;
		static constexpr bool verbose_jumps     = false;
		static constexpr bool verbose_registers = false;
#endif
		bool throw_on_unhandled_syscall = false;
		void system_call(int);
	private:
		bool m_stopped = false;
		std::map<int, syscall_t> m_syscall_handlers;
		static_assert((W == 4 || W == 8), "Must be either 4-byte or 8-byte ISA");
	};

#include "machine_inline.hpp"
}
