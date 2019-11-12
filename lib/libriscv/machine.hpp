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
	static constexpr int SYSCALL_EBREAK = 0;

	template <int W>
	struct Machine
	{
		using address_t = address_type<W>;          // one unsigned memory address
		using syscall_t = delegate<address_t (Machine<W>&)>;
		Machine(const std::vector<uint8_t>& binary, bool protect_memory = true);

		void simulate(uint64_t max_instructions = 0);
		void stop() noexcept;
		bool stopped() const noexcept;
		void install_syscall_handler(int, syscall_t);
		void reset();

		CPU<W>    cpu;
		Memory<W> memory;

		// copy data into the guests memory
		address_t copy_to_guest(address_t dst, const void* buf, size_t length);
		// push something onto the stack, and move the stack pointer
		address_t stack_push(const void* data, size_t length);

		// push all strings on stack and then create a mini-argv on SP
		void setup_argv(const std::vector<std::string>& args);

		// retrieve arguments during a system call
		template <typename T>
		inline T sysarg(int arg) const;

		// calls into the virtual machine, returning the value returned from
		// @function_name, which must be visible in the ELF symbol tables.
		// the function must use the C ABI calling convention.
		// NOTE: overwrites the exit (93) system call and relies on _exit
		// to stop execution right after returning. _exit must call the exit
		// (93) system call and not call destructors, which is the norm.
		long vmcall(const std::string& function_name,
					address_t a0 = 0, address_t a1 = 0, address_t a2 = 0);

		// sets up a function call only, executes no instructions
		void setup_call(const std::string& callsym, const std::string& retsym,
						address_t a0, address_t a1, address_t a2);

#ifdef RISCV_DEBUG
		void break_now();
		// immediately block execution, print registers and current instruction
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
