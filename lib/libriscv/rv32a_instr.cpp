#include "rv32i.hpp"

#define ATOMIC_INSTR(x, ...) \
		static CPU<4>::instruction_t instr32a_##x { __VA_ARGS__ }
#define DECODED_ATOMIC(x) instr32a_##x

namespace riscv
{
	ATOMIC_INSTR(AMOADD_W,
	[] (auto& cpu, rv32i_instruction instr)
	{
		if (instr.Atype.rs1 != 0)
		{
			// 1. load value from rs1
			const auto addr = cpu.reg(instr.Atype.rs1);
			auto value = cpu.machine().memory.template read<uint32_t> (addr);
			// 2. place value into rd
			if (instr.Atype.rd != 0) {
				cpu.reg(instr.Atype.rd) = value;
			}
			// 3. apply <add> to value and rs2
			value += cpu.reg(instr.Atype.rs2);
			// 4. write value back to [rs1]
			cpu.machine().memory.template write<uint32_t> (addr, value);
			return;
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		return snprintf(buffer, len, "AMOADD.W %s %s, %s",
                        RISCV::regname(instr.Atype.rs1),
                        RISCV::regname(instr.Atype.rs2),
                        RISCV::regname(instr.Atype.rd));
	});

    ATOMIC_INSTR(AMOSWAP_W,
	[] (auto& cpu, rv32i_instruction instr)
	{
		if (instr.Atype.rs1 != 0)
		{
			// 1. load value from rs1
			const auto addr = cpu.reg(instr.Atype.rs1);
			auto value = cpu.machine().memory.template read<uint32_t> (addr);
			// 2. place value into rd
			if (instr.Atype.rd != 0) {
				cpu.reg(instr.Atype.rd) = value;
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
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		return snprintf(buffer, len, "AMOSWAP.W %s %s, %s",
                        RISCV::regname(instr.Atype.rs1),
                        RISCV::regname(instr.Atype.rs2),
                        RISCV::regname(instr.Atype.rd));
	});

    ATOMIC_INSTR(LOAD_RESV,
	[] (auto& cpu, rv32i_instruction instr) {
		// handler
		if (instr.Atype.funct3 == 0x2 && instr.Atype.rs2 == 0)
		{
			const auto addr = cpu.reg(instr.Atype.rs1);
			cpu.atomics().load_reserve(addr);
			auto value = cpu.machine().memory.template read<uint32_t> (addr);
			cpu.reg(instr.Atype.rd) = value;
			return;
		}
        cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		return snprintf(buffer, len, "LR.W %s <- [%s]",
                        RISCV::regname(instr.Atype.rd),
                        RISCV::regname(instr.Atype.rs1));
	});

    ATOMIC_INSTR(STORE_COND,
	[] (auto& cpu, rv32i_instruction instr) {
		// handler
		if (instr.Atype.funct3 == 0x2 && instr.Atype.rs2 != 0)
		{
			const auto addr = cpu.reg(instr.Atype.rs1);
			const bool resv = cpu.atomics().store_conditional(addr);
			if (resv) {
				auto value = cpu.machine().memory.template read<uint32_t> (addr);
				cpu.reg(instr.Atype.rs2) = value;
			}
			cpu.reg(instr.Atype.rd) = (resv) ? 0 : -1;
			return;
		}
		cpu.trigger_exception(ILLEGAL_OPERATION);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) -> int {
		// printer
		return snprintf(buffer, len, "SC.W %s <- [%s], %s",
                        RISCV::regname(instr.Atype.rd),
                        RISCV::regname(instr.Atype.rs1),
                        RISCV::regname(instr.Atype.rs2));
	});
}
