#pragma once
#include <libriscv/machine.hpp>
#include <libriscv/rv32i_instr.hpp>
#include <cstdio>

namespace riscv
{
	template<int W>
	struct testable_insn {
		const char* name;     // test name
		address_type<W> bits; // the instruction bits
		const int reg;        // which register this insn affects
		const int index;      // test loop index
		address_type<W> initial_value; // start value of register
	};

	template <int W>
	static bool
	validate(Machine<W>& machine, const testable_insn<W>& insn,
			std::function<bool(CPU<W>&, const testable_insn<W>&)> callback)
	{
		static const address_type<W> MEMBASE = 0x1000;
		// create page, make it executable
		auto& page = machine.memory.create_writable_pageno(MEMBASE >> Page::SHIFT);
		page.attr.exec = true;
		page.attr.read = true; // needed for debugging
		// jump to beginning of page, write instruction
		machine.cpu.jump(MEMBASE);
		page.page().template aligned_write<uint32_t> (MEMBASE & (Page::size()-1), insn.bits);
		// execute instruction
		machine.cpu.reg(insn.reg) = insn.initial_value;
		machine.cpu.step_one();
		// call instruction validation callback
		if ( callback(machine.cpu, insn) ) return true;
		fprintf(stderr, "Failed test: %s on iteration %d\n", insn.name, insn.index);
		fprintf(stderr, "Register value: 0x%X\n", machine.cpu.reg(insn.reg));
		return false;
	}
}
