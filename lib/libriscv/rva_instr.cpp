#include "rv32i.hpp"
#include "instr_helpers.hpp"
static const char atomic_type[] { '?', '?', 'W', 'D' };
static const char* atomic_name2[] {
	"AMOADD", "AMOXOR", "AMOOR", "AMOAND", "AMOMIN", "AMOMAX", "AMOMINU", "AMOMAXU"
};

namespace riscv
{
	template <int W>
	template <typename Type>
	inline void CPU<W>::amo(format_t instr,
		void(*op)(CPU&, register_type<W>&, uint32_t))
	{
		// 1. load address from rs1
		const auto addr = this->reg(instr.Atype.rs1);
		// 2. read value from writable memory location
		auto& mem = machine().memory.template writable_read<Type> (addr);
		// 4. apply <op>
		register_type<W> value = mem;
		op(*this, value, instr.Atype.rs2);
		// 3. place value into rd
		// NOTE: we have to do it in this order, because we can
		// clobber rs2 when writing to rd, if they are the same!
		if (instr.Atype.rd != 0) {
			if constexpr (sizeof(Type) == 4)
				this->reg(instr.Atype.rd) = (int32_t) mem;
			else
				this->reg(instr.Atype.rd) = mem;
		}
		// 5. write value back to [rs1]
		mem = value;
	}

	ATOMIC_INSTR(AMOADD_W,
	[] (auto& cpu, rv32i_instruction instr)
	{
		cpu.template amo<uint32_t>(instr,
		[] (auto& cpu, auto& value, auto rs2) {
			value += cpu.reg(rs2);
		});
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		return snprintf(buffer, len, "%s.%c [%s] %s, %s",
						atomic_name2[instr.Atype.funct5 >> 2],
						atomic_type[instr.Atype.funct3 & 3],
                        RISCV::regname(instr.Atype.rs1),
                        RISCV::regname(instr.Atype.rs2),
                        RISCV::regname(instr.Atype.rd));
	});

	ATOMIC_INSTR(AMOXOR_W,
	[] (auto& cpu, rv32i_instruction instr)
	{
		cpu.template amo<uint32_t>(instr,
		[] (auto& cpu, auto& value, auto rs2) {
			value ^= cpu.reg(rs2);
		});
	},
	DECODED_ATOMIC(AMOADD_W).printer);

	ATOMIC_INSTR(AMOOR_W,
	[] (auto& cpu, rv32i_instruction instr)
	{
		cpu.template amo<uint32_t>(instr,
		[] (auto& cpu, auto& value, auto rs2) {
			value |= cpu.reg(rs2);
		});
	},
	DECODED_ATOMIC(AMOADD_W).printer);

	ATOMIC_INSTR(AMOAND_W,
	[] (auto& cpu, rv32i_instruction instr)
	{
		cpu.template amo<uint32_t>(instr,
		[] (auto& cpu, auto& value, auto rs2) {
			value &= cpu.reg(rs2);
		});
	},
	DECODED_ATOMIC(AMOADD_W).printer);

	ATOMIC_INSTR(AMOADD_D,
	[] (auto& cpu, rv32i_instruction instr)
	{
		cpu.template amo<uint64_t>(instr,
		[] (auto& cpu, auto& value, auto rs2) {
			value += cpu.reg(rs2);
		});
	},
	DECODED_ATOMIC(AMOADD_W).printer);

	ATOMIC_INSTR(AMOXOR_D,
	[] (auto& cpu, rv32i_instruction instr)
	{
		cpu.template amo<uint64_t>(instr,
		[] (auto& cpu, auto& value, auto rs2) {
			value ^= cpu.reg(rs2);
		});
	},
	DECODED_ATOMIC(AMOADD_W).printer);

	ATOMIC_INSTR(AMOOR_D,
	[] (auto& cpu, rv32i_instruction instr)
	{
		cpu.template amo<uint64_t>(instr,
		[] (auto& cpu, auto& value, auto rs2) {
			value |= cpu.reg(rs2);
		});
	},
	DECODED_ATOMIC(AMOADD_W).printer);

	ATOMIC_INSTR(AMOAND_D,
	[] (auto& cpu, rv32i_instruction instr)
	{
		cpu.template amo<uint64_t>(instr,
		[] (auto& cpu, auto& value, auto rs2) {
			value &= cpu.reg(rs2);
		});
	},
	DECODED_ATOMIC(AMOADD_W).printer);

	ATOMIC_INSTR(AMOSWAP_W,
	[] (auto& cpu, rv32i_instruction instr)
	{
		cpu.template amo<uint32_t>(instr,
		[] (auto& cpu, auto& value, auto rs2) {
			if (rs2 != 0) {
				value = cpu.reg(rs2);
			} else {
				value = 0;
			}
		});
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		return snprintf(buffer, len, "AMOSWAP.%c [%s] %s, %s",
						atomic_type[instr.Atype.funct3 & 3],
                        RISCV::regname(instr.Atype.rs1),
                        RISCV::regname(instr.Atype.rs2),
                        RISCV::regname(instr.Atype.rd));
	});

	ATOMIC_INSTR(AMOSWAP_D,
	[] (auto& cpu, rv32i_instruction instr)
	{
		cpu.template amo<uint64_t>(instr,
		[] (auto& cpu, auto& value, auto rs2) {
			if (rs2 != 0) {
				value = cpu.reg(rs2);
			} else {
				value = 0;
			}
		});
	},
	DECODED_ATOMIC(AMOSWAP_W).printer);

    ATOMIC_INSTR(LOAD_RESV,
	[] (auto& cpu, rv32i_instruction instr) {
		const auto addr = cpu.reg(instr.Atype.rs1);
		// switch on atomic type
		if (instr.Atype.funct3 == 0x2)
		{
			cpu.atomics().load_reserve(4, addr);
			auto value = cpu.machine().memory.template read<uint32_t> (addr);
			if (instr.Atype.rd != 0)
				cpu.reg(instr.Atype.rd) = (int32_t) value;
		}
		else if (instr.Atype.funct3 == 0x3)
		{
			cpu.atomics().load_reserve(8, addr);
			auto value = cpu.machine().memory.template read<uint64_t> (addr);
			if (instr.Atype.rd != 0)
				cpu.reg(instr.Atype.rd) = value;
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		return snprintf(buffer, len, "LR.%c [%s], %s",
				atomic_type[instr.Atype.funct3 & 3],
				RISCV::regname(instr.Atype.rs1),
				RISCV::regname(instr.Atype.rd));
	});

    ATOMIC_INSTR(STORE_COND,
	[] (auto& cpu, rv32i_instruction instr) {
		const auto addr = cpu.reg(instr.Atype.rs1);
		bool resv;
		if (instr.Atype.funct3 == 0x2)
		{
			resv = cpu.atomics().store_conditional(4, addr);
			if (resv) {
				cpu.machine().memory.template write<uint32_t> (addr, cpu.reg(instr.Atype.rs2));
			}
		}
		else if (instr.Atype.funct3 == 0x3)
		{
			resv = cpu.atomics().store_conditional(8, addr);
			if (resv) {
				cpu.machine().memory.template write<uint64_t> (addr, cpu.reg(instr.Atype.rs2));
			}
		}
		if (instr.Atype.rd != 0)
			cpu.reg(instr.Atype.rd) = (resv) ? 0 : 1;
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) -> int {
		return snprintf(buffer, len, "SC.%c [%s], %s res=%s",
				atomic_type[instr.Atype.funct3 & 3],
				RISCV::regname(instr.Atype.rs1),
				RISCV::regname(instr.Atype.rs2),
				RISCV::regname(instr.Atype.rd));
	});
}
