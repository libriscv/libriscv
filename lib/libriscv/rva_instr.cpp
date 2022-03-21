#include "rv32i.hpp"
#include "instr_helpers.hpp"
#include <inttypes.h>
static const char atomic_type[] { '?', '?', 'W', 'D', 'Q', '?', '?', '?' };
static const char* atomic_name2[] {
	"AMOADD", "AMOXOR", "AMOOR", "AMOAND", "AMOMIN", "AMOMAX", "AMOMINU", "AMOMAXU"
};
#define AMOSIZE_W   0x2
#define AMOSIZE_D   0x3
#define AMOSIZE_Q   0x4


namespace riscv
{
	template <int W>
	template <typename Type>
	inline void CPU<W>::amo(format_t instr,
		Type(*op)(CPU&, Type&, uint32_t))
	{
		// 1. load address from rs1
		const auto addr = this->reg(instr.Atype.rs1);
		// 2. verify address alignment vs Type
		if (UNLIKELY(addr % sizeof(Type) != 0)) {
			trigger_exception(INVALID_ALIGNMENT, addr);
		}
		// 3. read value from writable memory location
		// TODO: Make Type unsigned to match other templates, avoiding spam
		Type& mem = machine().memory.template writable_read<Type> (addr);
		// 4. apply <op>, writing the value to mem and returning old value
		Type old_value = op(*this, mem, instr.Atype.rs2);
		// 5. place value into rd
		// NOTE: we have to do it in this order, because we can
		// clobber rs2 when writing to rd, if they are the same!
		if (instr.Atype.rd != 0) {
			this->reg(instr.Atype.rd) = old_value;
		}
	}

	ATOMIC_INSTR(AMOADD_W,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_COLDATTR
	{
		cpu.template amo<int32_t>(instr,
		[] (auto& cpu, auto& value, auto rs2) {
			return __sync_fetch_and_add(&value, cpu.reg(rs2));
		});
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		return snprintf(buffer, len, "%s.%c [%s] %s, %s",
						atomic_name2[instr.Atype.funct5 >> 2],
						atomic_type[instr.Atype.funct3 & 7],
                        RISCV::regname(instr.Atype.rs1),
                        RISCV::regname(instr.Atype.rs2),
                        RISCV::regname(instr.Atype.rd));
	});

	ATOMIC_INSTR(AMOXOR_W,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_COLDATTR
	{
		cpu.template amo<int32_t>(instr,
		[] (auto& cpu, auto& value, auto rs2) {
			return __sync_fetch_and_xor(&value, cpu.reg(rs2));
		});
	},
	DECODED_ATOMIC(AMOADD_W).printer);

	ATOMIC_INSTR(AMOOR_W,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_COLDATTR
	{
		cpu.template amo<int32_t>(instr,
		[] (auto& cpu, auto& value, auto rs2) {
			return __sync_fetch_and_or(&value, cpu.reg(rs2));
		});
	},
	DECODED_ATOMIC(AMOADD_W).printer);

	ATOMIC_INSTR(AMOAND_W,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_COLDATTR
	{
		cpu.template amo<int32_t>(instr,
		[] (auto& cpu, auto& value, auto rs2) {
			return __sync_fetch_and_and(&value, cpu.reg(rs2));
		});
	},
	DECODED_ATOMIC(AMOADD_W).printer);

	ATOMIC_INSTR(AMOADD_D,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_COLDATTR
	{
		cpu.template amo<int64_t>(instr,
		[] (auto& cpu, auto& value, auto rs2) {
			return __sync_fetch_and_add(&value, cpu.reg(rs2));
		});
	},
	DECODED_ATOMIC(AMOADD_W).printer);

	ATOMIC_INSTR(AMOXOR_D,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_COLDATTR
	{
		cpu.template amo<int64_t>(instr,
		[] (auto& cpu, auto& value, auto rs2) {
			return __sync_fetch_and_xor(&value, cpu.reg(rs2));
		});
	},
	DECODED_ATOMIC(AMOADD_W).printer);

	ATOMIC_INSTR(AMOOR_D,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_COLDATTR
	{
		cpu.template amo<int64_t>(instr,
		[] (auto& cpu, auto& value, auto rs2) {
			return __sync_fetch_and_or(&value, cpu.reg(rs2));
		});
	},
	DECODED_ATOMIC(AMOADD_W).printer);

	ATOMIC_INSTR(AMOAND_D,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_COLDATTR
	{
		cpu.template amo<int64_t>(instr,
		[] (auto& cpu, auto& value, auto rs2) {
			return __sync_fetch_and_and(&value, cpu.reg(rs2));
		});
	},
	DECODED_ATOMIC(AMOADD_W).printer);

	ATOMIC_INSTR(AMOADD_Q,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_COLDATTR
	{
		cpu.template amo<__int128_t>(instr,
		[] (auto& cpu, auto& value, auto rs2) {
			auto old_value = value;
			value += cpu.reg(rs2);
			return old_value;
		});
	},
	DECODED_ATOMIC(AMOADD_W).printer);

	ATOMIC_INSTR(AMOXOR_Q,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_COLDATTR
	{
		cpu.template amo<__int128_t>(instr,
		[] (auto& cpu, auto& value, auto rs2) {
			auto old_value = value;
			value ^= cpu.reg(rs2);
			return old_value;
		});
	},
	DECODED_ATOMIC(AMOADD_W).printer);

	ATOMIC_INSTR(AMOOR_Q,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_COLDATTR
	{
		cpu.template amo<__int128_t>(instr,
		[] (auto& cpu, auto& value, auto rs2) {
			auto old_value = value;
			value |= cpu.reg(rs2);
			return old_value;
		});
	},
	DECODED_ATOMIC(AMOADD_W).printer);

	ATOMIC_INSTR(AMOAND_Q,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_COLDATTR
	{
		cpu.template amo<__int128_t>(instr,
		[] (auto& cpu, auto& value, auto rs2) {
			auto old_value = value;
			value &= cpu.reg(rs2);
			return old_value;
		});
	},
	DECODED_ATOMIC(AMOADD_W).printer);

	ATOMIC_INSTR(AMOSWAP_W,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_COLDATTR
	{
		cpu.template amo<int32_t>(instr,
		[] (auto& cpu, auto& value, auto rs2) {
			auto old_value = value;
			value = cpu.reg(rs2);
			return old_value;
		});
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		return snprintf(buffer, len, "AMOSWAP.%c [%s] %s, %s",
						atomic_type[instr.Atype.funct3 & 7],
                        RISCV::regname(instr.Atype.rs1),
                        RISCV::regname(instr.Atype.rs2),
                        RISCV::regname(instr.Atype.rd));
	});

	ATOMIC_INSTR(AMOSWAP_D,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_COLDATTR
	{
		cpu.template amo<int64_t>(instr,
		[] (auto& cpu, auto& value, auto rs2) {
			auto old_value = value;
			value = cpu.reg(rs2);
			return old_value;
		});
	},
	DECODED_ATOMIC(AMOSWAP_W).printer);

	ATOMIC_INSTR(AMOSWAP_Q,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_COLDATTR
	{
		cpu.template amo<__int128_t>(instr,
		[] (auto& cpu, auto& value, auto rs2) {
			auto old_value = value;
			value = cpu.reg(rs2);
			return old_value;
		});
	},
	DECODED_ATOMIC(AMOSWAP_W).printer);

    ATOMIC_INSTR(LOAD_RESV,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_COLDATTR
	{
		const auto addr = cpu.reg(instr.Atype.rs1);
		// switch on atomic type
		if (instr.Atype.funct3 == AMOSIZE_W)
		{
			cpu.atomics().load_reserve(4, addr);
			auto value = cpu.machine().memory.template read<uint32_t> (addr);
			if (instr.Atype.rd != 0)
				cpu.reg(instr.Atype.rd) = (int32_t) value;
		}
		else if (instr.Atype.funct3 == AMOSIZE_D)
		{
			if constexpr (RVISGE64BIT(cpu)) {
				cpu.atomics().load_reserve(8, addr);
				auto value = cpu.machine().memory.template read<uint64_t> (addr);
				if (instr.Atype.rd != 0)
					cpu.reg(instr.Atype.rd) = value;
			} else
				cpu.trigger_exception(ILLEGAL_OPCODE);
		}
		else if (instr.Atype.funct3 == AMOSIZE_Q)
		{
			if constexpr (RVIS128BIT(cpu)) {
				cpu.atomics().load_reserve(16, addr);
				auto value = cpu.machine().memory.template read<__uint128_t> (addr);
				if (instr.Atype.rd != 0)
					cpu.reg(instr.Atype.rd) = value;
			} else
				cpu.trigger_exception(ILLEGAL_OPCODE);
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		const long addr = cpu.reg(instr.Atype.rs1);
		return snprintf(buffer, len, "LR.%c [%s = 0x%" PRIX64 "], %s",
				atomic_type[instr.Atype.funct3 & 7],
				RISCV::regname(instr.Atype.rs1), uint64_t(addr),
				RISCV::regname(instr.Atype.rd));
	});

    ATOMIC_INSTR(STORE_COND,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_COLDATTR
	{
		const auto addr = cpu.reg(instr.Atype.rs1);
		bool resv = false;
		if (instr.Atype.funct3 == AMOSIZE_W)
		{
			resv = cpu.atomics().store_conditional(4, addr);
			if (resv) {
				cpu.machine().memory.template write<uint32_t> (addr, cpu.reg(instr.Atype.rs2));
			}
		}
		else if (instr.Atype.funct3 == AMOSIZE_D)
		{
			if constexpr (RVISGE64BIT(cpu)) {
				resv = cpu.atomics().store_conditional(8, addr);
				if (resv) {
					cpu.machine().memory.template write<uint64_t> (addr, cpu.reg(instr.Atype.rs2));
				}
			} else
				cpu.trigger_exception(ILLEGAL_OPCODE);
		}
		else if (instr.Atype.funct3 == AMOSIZE_Q)
		{
			if constexpr (RVIS128BIT(cpu)) {
				resv = cpu.atomics().store_conditional(16, addr);
				if (resv) {
					cpu.machine().memory.template write<__uint128_t> (addr, cpu.reg(instr.Atype.rs2));
				}
			} else
				cpu.trigger_exception(ILLEGAL_OPCODE);
		}
		if (instr.Atype.rd != 0)
			cpu.reg(instr.Atype.rd) = (resv) ? 0 : 1;
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		return snprintf(buffer, len, "SC.%c [%s], %s res=%s",
				atomic_type[instr.Atype.funct3 & 7],
				RISCV::regname(instr.Atype.rs1),
				RISCV::regname(instr.Atype.rs2),
				RISCV::regname(instr.Atype.rd));
	});
}
