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
		Machine(std::vector<uint8_t> binary);

		void simulate();
		void stop() noexcept;
		bool stopped() const noexcept;
		void install_syscall_handler(int, syscall_t);
		void reset();

		CPU<W>    cpu;
		Memory<W> memory;

		// retrieve arguments during a system call
		template <typename T>
		inline T sysarg(int arg) const;

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

		void system_call(int);
	private:
		bool m_stopped = false;
		std::map<int, syscall_t> m_syscall_handlers;
		static_assert((W == 4 || W == 8), "Must be either 4-byte or 8-byte ISA");
	};

#include "machine_inline.hpp"
}
