#include "rv32i.hpp"
#include "instr_helpers.hpp"
static const char atomic_type[] { '?', '?', 'W', 'D' };

namespace riscv
{
	ATOMIC_INSTR(AMOADD,
	[] (auto& cpu, rv32i_instruction instr)
	{
		if (instr.Atype.rs1 != 0)
		{
			// 1. load value from rs1
			const auto addr = cpu.reg(instr.Atype.rs1);
			auto value = cpu.machine().memory.template read<uint32_t> (addr);
			// 2. place value into rd
			if (instr.Atype.rd != 0) {
				cpu.reg(instr.Atype.rd) = (int32_t) value;
			}
			// 3. apply <add> to value and rs2
			value += cpu.reg(instr.Atype.rs2);
			// 4. write value back to [rs1]
			cpu.machine().memory.template write<uint32_t> (addr, value);
			return;
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		return snprintf(buffer, len, "AMOADD.%c %s %s, %s",
						atomic_type[instr.Atype.funct3 & 3],
                        RISCV::regname(instr.Atype.rs1),
                        RISCV::regname(instr.Atype.rs2),
                        RISCV::regname(instr.Atype.rd));
	});

    ATOMIC_INSTR(AMOSWAP,
	[] (auto& cpu, rv32i_instruction instr)
	{
		if (instr.Atype.rs1 != 0)
		{
			// 1. load value from rs1
			const auto addr = cpu.reg(instr.Atype.rs1);
			RVREGTYPE(cpu) value = cpu.machine().memory.template read<uint32_t> (addr);
			// 2. place value into rd
			if (instr.Atype.rd != 0) {
				cpu.reg(instr.Atype.rd) = (int32_t) value;
			}
			// 3. apply <swap> to value and rs2
			if (instr.Atype.rs2 != 0) {
				std::swap(value, cpu.reg(instr.Atype.rs2));
			}
			else {
				value = 0;
			}
			// 4. write value back to [rs1]
			cpu.machine().memory.template write<uint32_t> (addr, value);
			return;
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		return snprintf(buffer, len, "AMOSWAP.%c %s %s, %s",
						atomic_type[instr.Atype.funct3 & 3],
                        RISCV::regname(instr.Atype.rs1),
                        RISCV::regname(instr.Atype.rs2),
                        RISCV::regname(instr.Atype.rd));
	});

	ATOMIC_INSTR(AMOOR,
	[] (auto& cpu, rv32i_instruction instr)
	{
		if (instr.Atype.rs1 != 0)
		{
			// 1. load value from rs1
			const auto addr = cpu.reg(instr.Atype.rs1);
			auto value = cpu.machine().memory.template read<uint32_t> (addr);
			// 2. place value into rd
			if (instr.Atype.rd != 0) {
				cpu.reg(instr.Atype.rd) = (int32_t) value;
			}
			// 3. apply <or> to value and rs2
			value |= cpu.reg(instr.Atype.rs2);
			// 4. write value back to [rs1]
			cpu.machine().memory.template write<uint32_t> (addr, value);
			return;
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		return snprintf(buffer, len, "AMOOR.%c %s %s, %s",
						atomic_type[instr.Atype.funct3 & 3],
                        RISCV::regname(instr.Atype.rs1),
                        RISCV::regname(instr.Atype.rs2),
                        RISCV::regname(instr.Atype.rd));
	});

    ATOMIC_INSTR(LOAD_RESV,
	[] (auto& cpu, rv32i_instruction instr) {
		const auto addr = cpu.reg(instr.Atype.rs1);
		cpu.atomics().load_reserve(addr);
		// switch on atomic type
		if (instr.Atype.funct3 == 0x2 && instr.Atype.rs2 == 0)
		{
			auto value = cpu.machine().memory.template read<uint32_t> (addr);
			if (instr.Atype.rd != 0)
				cpu.reg(instr.Atype.rd) = (int32_t) value;
		}
		else if (instr.Atype.funct3 == 0x3 && instr.Atype.rs2 == 0)
		{
			auto value = cpu.machine().memory.template read<uint64_t> (addr);
			if (instr.Atype.rd != 0)
				cpu.reg(instr.Atype.rd) = value;
		} else {
        	cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		return snprintf(buffer, len, "LR.%c %s <- [%s]",
						atomic_type[instr.Atype.funct3 & 3],
                        RISCV::regname(instr.Atype.rd),
                        RISCV::regname(instr.Atype.rs1));
	});

    ATOMIC_INSTR(STORE_COND,
	[] (auto& cpu, rv32i_instruction instr) {
		const auto addr = cpu.reg(instr.Atype.rs1);
		const bool resv = cpu.atomics().store_conditional(addr);
		// store conditionally
		if (instr.Atype.funct3 == 0x2 && instr.Atype.rs2 != 0)
		{
			if (resv) {
				auto value = cpu.machine().memory.template read<uint32_t> (addr);
				cpu.reg(instr.Atype.rs2) = (int32_t) value;
			}
			if (instr.Atype.rd != 0)
				cpu.reg(instr.Atype.rd) = (resv) ? 0 : -1;
		}
		else if (instr.Atype.funct3 == 0x3 && instr.Atype.rs2 != 0)
		{
			if (resv) {
				auto value = cpu.machine().memory.template read<uint64_t> (addr);
				cpu.reg(instr.Atype.rs2) = value;
			}
			if (instr.Atype.rd != 0)
				cpu.reg(instr.Atype.rd) = (resv) ? 0 : -1;
		} else {
			cpu.trigger_exception(ILLEGAL_OPERATION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		return snprintf(buffer, len, "SC.%c %s <- [%s], %s",
						atomic_type[instr.Atype.funct3 & 3],
                        RISCV::regname(instr.Atype.rd),
                        RISCV::regname(instr.Atype.rs1),
                        RISCV::regname(instr.Atype.rs2));
	});
}
