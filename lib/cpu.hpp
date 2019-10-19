#pragma once
#include "types.hpp"
#include "instruction.hpp"
#include "rv32i.hpp"

namespace riscv
{
	template<int W> struct Machine;

	template<int W>
	struct CPU
	{
		using address_t = address_type<W>;          // one unsigned memory address
		using isa_t     = isa_type<W>;              // 32- or 64-bit architecture
		using format_t  = typename isa_t::format_t; // one machine instruction

		struct instruction_t {
			using handler_t = void (*)(CPU<W>&, format_t);
			using printer_t = int  (*)(char*, size_t, CPU<W>&, format_t);

			const handler_t handler; // callback for executing one instruction
			const printer_t printer; // callback for logging one instruction
		};

		void simulate();
		void reset();

		const auto& registers() const noexcept { return m_data.m_regs; }
		auto& registers() noexcept { return m_data.m_regs; }

		address_t pc() const noexcept { return m_data.m_pc; }

		CPU(Machine<W>&);
	private:
		using decoded_t = std::tuple<instruction_t&, format_t>;
		struct {
			address_t m_pc = 0;
			std::array<typename isa_t::register_t, 32> m_regs;
		} m_data;

		void execute();
		inline decoded_t decode(address_t);

		Machine<W>& m_machine;
		static_assert((W == 4 || W == 8), "Must be either 4-byte or 8-byte ISA");
	};

#include "cpu_inline.hpp"
}
