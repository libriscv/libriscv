#include "instr_helpers.hpp"
#include "instruction_list.hpp"
#include "internal_common.hpp"
#include "rvc.hpp"
#include <atomic>
#if __has_include(<bit>)
# include <bit>
# if defined(__cpp_lib_bitops)
#  define RISCV_HAS_BITOPS
# endif
#endif
#include <inttypes.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace riscv
{
// MULH/MULHU/MULHSU helpers in internal_common.hpp, shared with RVV.
#ifdef _MSC_VER
#define bswap32(x)   _byteswap_ulong(x)
#define bswap64(x)   _byteswap_uint64(x)
#else
#ifndef bswap32
#define bswap32(x)   __builtin_bswap32(x)
#define bswap64(x)   __builtin_bswap64(x)
#endif
#endif // _MSC_VER

	template <typename T>
	static inline T rv_brev8(T value) noexcept
	{
		T result = 0;
		for (size_t i = 0; i < sizeof(T); i++) {
			uint8_t b = uint8_t(value >> (i * 8));
			b = uint8_t(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
			b = uint8_t(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
			b = uint8_t(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
			result |= T(b) << (i * 8);
		}
		return result;
	}

	static inline uint32_t rv_zip32(uint32_t value) noexcept
	{
		uint32_t lo = value & 0xFFFF;
		uint32_t hi = value >> 16;
		lo = (lo | (lo << 8)) & 0x00FF00FF;
		lo = (lo | (lo << 4)) & 0x0F0F0F0F;
		lo = (lo | (lo << 2)) & 0x33333333;
		lo = (lo | (lo << 1)) & 0x55555555;
		hi = (hi | (hi << 8)) & 0x00FF00FF;
		hi = (hi | (hi << 4)) & 0x0F0F0F0F;
		hi = (hi | (hi << 2)) & 0x33333333;
		hi = (hi | (hi << 1)) & 0x55555555;
		return lo | (hi << 1);
	}

	static inline uint32_t rv_unzip32(uint32_t value) noexcept
	{
		uint32_t lo = value & 0x55555555;
		uint32_t hi = (value >> 1) & 0x55555555;
		lo = (lo | (lo >> 1)) & 0x33333333;
		lo = (lo | (lo >> 2)) & 0x0F0F0F0F;
		lo = (lo | (lo >> 4)) & 0x00FF00FF;
		lo = (lo | (lo >> 8)) & 0x0000FFFF;
		hi = (hi | (hi >> 1)) & 0x33333333;
		hi = (hi | (hi >> 2)) & 0x0F0F0F0F;
		hi = (hi | (hi >> 4)) & 0x00FF00FF;
		hi = (hi | (hi >> 8)) & 0x0000FFFF;
		return lo | (hi << 16);
	}

	INSTRUCTION(NOP,
	[] (auto& /* cpu */, rv32i_instruction /* instr */) RVINSTR_COLDATTR {
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return rv_print_hint(buffer, len, instr,
			RVISGE64BIT(cpu), RVIS128BIT(cpu), cpu.pc());
	});

	INSTRUCTION(UNIMPLEMENTED,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_COLDATTR {
		if (instr.length() == 4)
			cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION, instr.whole);
		else
			cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION, instr.half[0]);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVPRINT::illegal(buffer, len, instr.whole, instr.length());
	});

	INSTRUCTION(ILLEGAL,
	[] (auto& cpu, rv32i_instruction /* instr */) RVINSTR_COLDATTR {
		cpu.trigger_exception(ILLEGAL_OPCODE);
	}, DECODED_INSTR(UNIMPLEMENTED).printer);

	INSTRUCTION(LOAD_I8,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		auto& reg = cpu.reg(instr.Itype.rd);
		const auto addr = cpu.reg(instr.Itype.rs1) + RVIMM(cpu, instr.Itype);
		reg = (int8_t) cpu.machine().memory.template read<uint8_t>(addr);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVDISASM::op_load(buffer, len, instr, RVIS128BIT(cpu));
	});

	INSTRUCTION(LOAD_I16,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		auto& reg = cpu.reg(instr.Itype.rd);
		const auto addr = cpu.reg(instr.Itype.rs1) + RVIMM(cpu, instr.Itype);
		reg = (int16_t) cpu.machine().memory.template read<uint16_t>(addr);
	}, DECODED_INSTR(LOAD_I8).printer);

	INSTRUCTION(LOAD_I32,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		auto& reg = cpu.reg(instr.Itype.rd);
		const auto addr = cpu.reg(instr.Itype.rs1) + RVIMM(cpu, instr.Itype);
		reg = (int32_t) cpu.machine().memory.template read<uint32_t>(addr);
	}, DECODED_INSTR(LOAD_I8).printer);

	INSTRUCTION(LOAD_I64,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		auto& reg = cpu.reg(instr.Itype.rd);
		const auto addr = cpu.reg(instr.Itype.rs1) + RVIMM(cpu, instr.Itype);
		reg = (int64_t) cpu.machine().memory.template read<uint64_t>(addr);
	}, DECODED_INSTR(LOAD_I8).printer);

	INSTRUCTION(LOAD_U8,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		auto& reg = cpu.reg(instr.Itype.rd);
		const auto addr = cpu.reg(instr.Itype.rs1) + RVIMM(cpu, instr.Itype);
		reg = (RVSIGNTYPE(cpu)) cpu.machine().memory.template read<uint8_t>(addr);
	}, DECODED_INSTR(LOAD_I8).printer);

	INSTRUCTION(LOAD_U16,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		auto& reg = cpu.reg(instr.Itype.rd);
		const auto addr = cpu.reg(instr.Itype.rs1) + RVIMM(cpu, instr.Itype);
		reg = (RVSIGNTYPE(cpu)) cpu.machine().memory.template read<uint16_t>(addr);
	}, DECODED_INSTR(LOAD_I8).printer);

	INSTRUCTION(LOAD_U32,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		auto& reg = cpu.reg(instr.Itype.rd);
		const auto addr = cpu.reg(instr.Itype.rs1) + RVIMM(cpu, instr.Itype);
		reg = (RVSIGNTYPE(cpu)) cpu.machine().memory.template read<uint32_t>(addr);
	}, DECODED_INSTR(LOAD_I8).printer);

	INSTRUCTION(LOAD_U64,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		auto& reg = cpu.reg(instr.Itype.rd);
		const auto addr = cpu.reg(instr.Itype.rs1) + RVIMM(cpu, instr.Itype);
		reg = (RVSIGNTYPE(cpu)) cpu.machine().memory.template read<uint64_t>(addr);
	}, DECODED_INSTR(LOAD_I8).printer);

	INSTRUCTION(LOAD_X_DUMMY,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_COLDATTR
	{
		auto addr = cpu.reg(instr.Itype.rs1) + RVIMM(cpu, instr.Itype);
		switch (instr.Itype.funct3) {
		case 0x0:
			cpu.machine().memory.template read<uint8_t>(addr);
			return;
		case 0x1:
			cpu.machine().memory.template read<uint16_t>(addr);
			return;
		case 0x2:
			cpu.machine().memory.template read<uint32_t>(addr);
			return;
		case 0x3:
			if constexpr (RVISGE64BIT(cpu)) {
				cpu.machine().memory.template read<uint64_t>(addr);
				return;
			}
			cpu.trigger_exception(ILLEGAL_OPCODE);
		case 0x7:
			if constexpr (RVIS128BIT(cpu)) {
				addr &= ~RVREGTYPE(cpu)(0xF);
				cpu.machine().memory.template read<RVREGTYPE(cpu)>(addr);
				return;
			}
			cpu.trigger_exception(ILLEGAL_OPCODE);
		}
	}, DECODED_INSTR(LOAD_I8).printer);

	INSTRUCTION(STORE_I8_IMM,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const auto& value = cpu.reg(instr.Stype.rs2);
		const auto addr  = cpu.reg(instr.Stype.rs1) + RVIMM(cpu, instr.Stype);
		cpu.machine().memory.template write<uint8_t>(addr, value);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVDISASM::op_store(buffer, len, instr, RVIS128BIT(cpu));
	});

	INSTRUCTION(STORE_I8,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const auto& addr  = cpu.reg(instr.Stype.rs1);
		const auto& value = cpu.reg(instr.Stype.rs2);
		cpu.machine().memory.template write<uint8_t>(addr, value);
	}, DECODED_INSTR(STORE_I8_IMM).printer);

	INSTRUCTION(STORE_I16_IMM,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const auto& value = cpu.reg(instr.Stype.rs2);
		const auto addr  = cpu.reg(instr.Stype.rs1) + RVIMM(cpu, instr.Stype);
		cpu.machine().memory.template write<uint16_t>(addr, value);
	}, DECODED_INSTR(STORE_I8_IMM).printer);

	INSTRUCTION(STORE_I32_IMM,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const auto& value = cpu.reg(instr.Stype.rs2);
		const auto addr  = cpu.reg(instr.Stype.rs1) + RVIMM(cpu, instr.Stype);
		cpu.machine().memory.template write<uint32_t>(addr, value);
	}, DECODED_INSTR(STORE_I8_IMM).printer);

	INSTRUCTION(STORE_I64_IMM,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const auto& value = cpu.reg(instr.Stype.rs2);
		const auto addr  = cpu.reg(instr.Stype.rs1) + RVIMM(cpu, instr.Stype);
		cpu.machine().memory.template write<uint64_t>(addr, value);
	}, DECODED_INSTR(STORE_I8_IMM).printer);

	INSTRUCTION(STORE_I128_IMM,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const auto& value = cpu.reg(instr.Stype.rs2);
		auto addr = cpu.reg(instr.Stype.rs1) + RVIMM(cpu, instr.Stype);
		addr &= ~RVREGTYPE(cpu)(0xF);
		cpu.machine().memory.template write<RVREGTYPE(cpu)>(addr, value);
	}, DECODED_INSTR(STORE_I8_IMM).printer);

#define VERBOSE_BRANCH() \
	if constexpr (verbose_branches_enabled) { \
		printf(">>> BRANCH jump to 0x%" PRIX64 "\n", uint64_t(cpu.pc() + 4)); \
	}

	INSTRUCTION(BRANCH_EQ,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const auto reg1 = cpu.reg(instr.Btype.rs1);
		const auto reg2 = cpu.reg(instr.Btype.rs2);
		if (reg1 == reg2) {
			cpu.jump(cpu.pc() + RVIMM(cpu, instr.Btype) - 4);
			VERBOSE_BRANCH()
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVDISASM::op_branch(buffer, len, instr, cpu.pc(), RVISGE64BIT(cpu));
	});

	INSTRUCTION(BRANCH_NE,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const auto reg1 = cpu.reg(instr.Btype.rs1);
		const auto reg2 = cpu.reg(instr.Btype.rs2);
		if (reg1 != reg2) {
			cpu.jump(cpu.pc() + RVIMM(cpu, instr.Btype) - 4);
			VERBOSE_BRANCH()
		}
	}, DECODED_INSTR(BRANCH_EQ).printer);

	INSTRUCTION(BRANCH_LT,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const auto reg1 = cpu.reg(instr.Btype.rs1);
		const auto reg2 = cpu.reg(instr.Btype.rs2);
		if (RVTOSIGNED(reg1) < RVTOSIGNED(reg2)) {
			cpu.jump(cpu.pc() + RVIMM(cpu, instr.Btype) - 4);
			VERBOSE_BRANCH()
		}
	}, DECODED_INSTR(BRANCH_EQ).printer);

	INSTRUCTION(BRANCH_GE,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const auto reg1 = cpu.reg(instr.Btype.rs1);
		const auto reg2 = cpu.reg(instr.Btype.rs2);
		if (RVTOSIGNED(reg1) >= RVTOSIGNED(reg2)) {
			cpu.jump(cpu.pc() + RVIMM(cpu, instr.Btype) - 4);
			VERBOSE_BRANCH()
		}
	}, DECODED_INSTR(BRANCH_EQ).printer);

	INSTRUCTION(BRANCH_LTU,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const auto& reg1 = cpu.reg(instr.Btype.rs1);
		const auto& reg2 = cpu.reg(instr.Btype.rs2);
		if (reg1 < reg2) {
			cpu.jump(cpu.pc() + RVIMM(cpu, instr.Btype) - 4);
			VERBOSE_BRANCH()
		}
	}, DECODED_INSTR(BRANCH_EQ).printer);

	INSTRUCTION(BRANCH_GEU,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		const auto& reg1 = cpu.reg(instr.Btype.rs1);
		const auto& reg2 = cpu.reg(instr.Btype.rs2);
		if (reg1 >= reg2) {
			cpu.jump(cpu.pc() + RVIMM(cpu, instr.Btype) - 4);
			VERBOSE_BRANCH()
		}
	}, DECODED_INSTR(BRANCH_EQ).printer);

	INSTRUCTION(JALR,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		// jump to register + immediate
		// NOTE: if rs1 == rd, avoid clobber by storing address first
		const auto address = cpu.reg(instr.Itype.rs1) + RVIMM(cpu, instr.Itype);
		// Link *next* instruction (rd = PC + 4)
		if (LIKELY(instr.Itype.rd != 0)) {
			cpu.reg(instr.Itype.rd) = cpu.pc() + 4;
		}
		cpu.jump(address - 4);
		if constexpr (verbose_branches_enabled) {
		printf(">>> JMP 0x%" PRIX64 " <-- %s = 0x%" PRIX64 "%+d\n",
				uint64_t(address),
				RISCV::regname(instr.Itype.rs1),
				uint64_t(cpu.reg(instr.Itype.rs1)),
				instr.Itype.signed_imm());
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVDISASM::op_jalr(buffer, len, instr);
	});

	INSTRUCTION(JAL,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		// Link *next* instruction (rd = PC + 4)
		cpu.reg(instr.Jtype.rd) = cpu.pc() + 4;
		// And jump relative
		cpu.jump(cpu.pc() + instr.Jtype.jump_offset() - 4);
		if constexpr (verbose_branches_enabled) {
			printf(">>> CALL 0x%" PRIX64 " <-- %s = 0x%" PRIX64 "\n",
					uint64_t(cpu.pc()),
					RISCV::regname(instr.Jtype.rd),
					uint64_t(cpu.reg(instr.Jtype.rd)));
		}
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVDISASM::op_jal(buffer, len, instr, cpu.pc(), RVISGE64BIT(cpu));
	});

	INSTRUCTION(JMPI,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		// Jump relative
		cpu.jump(cpu.pc() + instr.Jtype.jump_offset() - 4);
		if constexpr (verbose_branches_enabled) {
			printf(">>> JMP 0x%" PRIX64 " <-- %s = 0x%" PRIX64 "\n",
					uint64_t(cpu.pc()),
					RISCV::regname(instr.Jtype.rd),
					uint64_t(cpu.reg(instr.Jtype.rd)));
		}
	}, DECODED_INSTR(JAL).printer);

	INSTRUCTION(OP_IMM,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		auto& dst = cpu.reg(instr.Itype.rd);
		const auto src = cpu.reg(instr.Itype.rs1);
		const bool rv32_shamt_legal = !RVIS32BIT(cpu) || (instr.Itype.imm & 0x20) == 0;
		switch (instr.Itype.funct3) {
		case 0x1: // *NOT* SLLI, SEXT.B, SEXT.H, CTZ, CLZ, CPOP
			switch (instr.Itype.imm) {
			case 0b011000000100: // SEXT.B
				dst = RVSIGNTYPE(cpu)(int8_t(src));
				return;
			case 0b011000000101: // SEXT.H
				dst = RVSIGNTYPE(cpu)(int16_t(src));
				return;
			case 0b011000000000: // CLZ
#ifdef RISCV_HAS_BITOPS
				dst = std::countl_zero(src);
#else
				if constexpr (RVIS32BIT(cpu))
					dst = src ? __builtin_clz(src) : RVXLEN(cpu);
				else
					dst = src ? __builtin_clzl(src) : RVXLEN(cpu);
#endif
				return;
			case 0b011000000001: // CTZ
#ifdef RISCV_HAS_BITOPS
				dst = std::countr_zero(src);
#else
				if constexpr (RVIS32BIT(cpu))
					dst = src ? __builtin_ctz(src) : 0;
				else
					dst = src ? __builtin_ctzl(src) : 0;
#endif
				return;
			case 0b011000000010: // CPOP
#ifdef RISCV_HAS_BITOPS
				dst = std::popcount(src);
#else
				if constexpr (RVIS32BIT(cpu))
					dst = __builtin_popcount(src);
				else
					dst = __builtin_popcountl(src);
#endif
				return;
			case 0b000010001111:
				if constexpr (RVIS32BIT(cpu)) {
					dst = rv_zip32(uint32_t(src));
					return;
				}
				break;
			default:
				if (instr.Itype.high_bits() == 0x280 && rv32_shamt_legal) {
					// BSETI: Bit-set immediate
					dst = src | (RVREGTYPE(cpu)(1) << (instr.Itype.imm & (RVXLEN(cpu)-1)));
					return;
				}
				else if (instr.Itype.high_bits() == 0x480 && rv32_shamt_legal) {
					// BCLRI: Bit-clear immediate
					dst = src & ~(RVREGTYPE(cpu)(1) << (instr.Itype.imm & (RVXLEN(cpu)-1)));
					return;
				}
				else if (instr.Itype.high_bits() == 0x680 && rv32_shamt_legal) {
					// BINVI: Bit-invert immediate
					dst = src ^ (RVREGTYPE(cpu)(1) << (instr.Itype.imm & (RVXLEN(cpu)-1)));
					return;
				}
			}
			break;
		case 0x2: // SLTI: Set less than immediate
			dst = (RVTOSIGNED(src) < RVIMM(cpu, instr.Itype));
			return;
		case 0x3: // SLTIU: Sign-extend, then treat as unsigned
			dst = (src < (RVREGTYPE(cpu)) RVIMM(cpu, instr.Itype));
			return;
		case 0x4: // XORI:
			dst = src ^ RVIMM(cpu, instr.Itype);
			return;
		case 0x5: // SRLI / SRAI / RORI / ORC.B
			if (instr.Itype.is_srai() && rv32_shamt_legal) {
				// SRAI: Preserve the sign bit
				dst = (RVSIGNTYPE(cpu))src >> (instr.Itype.imm & (RVXLEN(cpu)-1));
				return;
			}
			else if (instr.Itype.is_rori() && rv32_shamt_legal) {
				// RORI: Rotate right. Mask the complementary count, as a
				// zero rotate would otherwise shift by the full width
				const auto shift = instr.Itype.imm & (RVXLEN(cpu) - 1);
				dst = (src >> shift) | (src << ((RVXLEN(cpu) - shift) & (RVXLEN(cpu) - 1)));
				return;
			}
			else if (instr.Itype.high_bits() == 0x480 && rv32_shamt_legal) {
				// BEXTI: Single-bit Extract
				dst = (src >> (instr.Itype.imm & (RVXLEN(cpu)-1))) & 1;
				return;
			}
			else if (instr.Itype.imm == 0x287) {
				// ORC.B: Bitwise OR-combine
				auto* src_bytes = (char *)&src;
				auto* dst_bytes = (char *)&dst;
				for (size_t i = 0; i < sizeof(src); i++)
					dst_bytes[i] = src_bytes[i] ? 0xFF : 0x0;
				return;
			}
			else if (instr.Itype.is_rev8<sizeof(dst)>()) {
				// REV8: Byte-reverse register
				if constexpr (RVIS32BIT(cpu))
					dst = bswap32(src);
				else
					dst = bswap64(src);
				return;
			}
			else if (instr.Itype.imm == 0b011010000111) {
				dst = rv_brev8(RVREGTYPE(cpu)(src));
				return;
			}
			else if (instr.Itype.imm == 0b000010001111) {
				if constexpr (RVIS32BIT(cpu)) {
					dst = rv_unzip32(uint32_t(src));
					return;
				}
			}
			break;
		case 0x6: // ORI: Or sign-extended 12-bit immediate
			dst = src | RVIMM(cpu, instr.Itype);
			return;
		}
		cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION, instr.whole);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVDISASM::op_imm(buffer, len, instr, RVISGE64BIT(cpu));
	});

	INSTRUCTION(OP_IMM_ADDI,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		// ADDI: Add sign-extended 12-bit immediate
		cpu.reg(instr.Itype.rd) =
			cpu.reg(instr.Itype.rs1) + RVIMM(cpu, instr.Itype);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVDISASM::rri(buffer, len, "addi", instr);
	});

	INSTRUCTION(OP_IMM_LI,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		// LI: Load sign-extended 12-bit immediate
		cpu.reg(instr.Itype.rd) = (RVSIGNTYPE(cpu)) RVIMM(cpu, instr.Itype);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVDISASM::rri(buffer, len, "addi", instr);
	});

	INSTRUCTION(OP_MV,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		cpu.reg(instr.Itype.rd) = cpu.reg(instr.Itype.rs1);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVDISASM::rri(buffer, len, "addi", instr);
	});

	// The reserved RV32 imm[5]=1 encodings never reach here: the decoder
	// (instr_decoding.inc) routes them to the generic OP_IMM handler, which
	// rejects them. Keep this handler free of the extra branch.
	INSTRUCTION(OP_IMM_SLLI,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		auto& dst = cpu.reg(instr.Itype.rd);
		const auto src = cpu.reg(instr.Itype.rs1);
		// SLLI: Logical left-shift 5/6/7-bit immediate
		dst = src << (instr.Itype.imm & (RVXLEN(cpu)-1));
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVDISASM::rrs(buffer, len, "slli", instr,
			instr.Itype.imm & (RVISGE64BIT(cpu) ? 0x3F : 0x1F));
	});

	// See OP_IMM_SLLI above: reserved RV32 encodings are filtered at decode.
	INSTRUCTION(OP_IMM_SRLI,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		auto& dst = cpu.reg(instr.Itype.rd);
		const auto src = cpu.reg(instr.Itype.rs1);
		// SRLI: Shift-right logical 5/6/7-bit immediate
		dst = src >> (instr.Itype.imm & (RVXLEN(cpu)-1));
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVDISASM::rrs(buffer, len, "srli", instr,
			instr.Itype.imm & (RVISGE64BIT(cpu) ? 0x3F : 0x1F));
	});

	INSTRUCTION(OP_IMM_ANDI,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		auto& dst = cpu.reg(instr.Itype.rd);
		// ANDI: And sign-extended 12-bit immediate
		dst = cpu.reg(instr.Itype.rs1) & RVIMM(cpu, instr.Itype);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVDISASM::rri(buffer, len, "andi", instr);
	});

	INSTRUCTION(OP,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		auto& dst = cpu.reg(instr.Rtype.rd);
		const auto src1 = cpu.reg(instr.Rtype.rs1);
		const auto src2 = cpu.reg(instr.Rtype.rs2);

		switch (instr.Rtype.jumptable_friendly_op()) {
		case 0x1: // SLL
			dst = src1 << (src2 & (RVXLEN(cpu)-1));
			return;
		case 0x2: // SLT
			dst = (RVTOSIGNED(src1) < RVTOSIGNED(src2));
			return;
		case 0x3: // SLTU
			dst = (src1 < src2);
			return;
		case 0x4: // XOR
			dst = src1 ^ src2;
			return;
		case 0x5: // SRL: Logical right shift
			dst = src1 >> (src2 & (RVXLEN(cpu)-1));
			return;
		case 0x6: // OR
			dst = src1 | src2;
			return;
		case 0x7: // AND
			dst = src1 & src2;
			return;
		// extension RV32M / RV64M
		case 0x10: // MUL
			// MUL keeps the low XLEN bits, which is the same product either
			// way, but only the unsigned multiplication is defined on overflow
			dst = RVREGTYPE(cpu)(src1) * RVREGTYPE(cpu)(src2);
			return;
		case 0x11: // MULH (signed x signed)
			if constexpr (RVIS32BIT(cpu)) {
				dst = uint64_t((int64_t)RVTOSIGNED(src1) * (int64_t)RVTOSIGNED(src2)) >> 32u;
			} else if constexpr (RVIS64BIT(cpu)) {
				dst = mulhi64(src1, src2);
			} else {
				dst = 0;
			}
			return;
		case 0x12: // MULHSU (signed x unsigned)
			if constexpr (RVIS32BIT(cpu)) {
				dst = uint64_t((int64_t)RVTOSIGNED(src1) * (uint64_t)src2) >> 32u;
			} else if constexpr (RVIS64BIT(cpu)) {
				dst = mulhsu64(src1, src2);
			} else {
				dst = 0;
			}
			return;
		case 0x13: // MULHU (unsigned x unsigned)
			if constexpr (RVIS32BIT(cpu)) {
				dst = uint64_t((uint64_t)src1 * (uint64_t)src2) >> 32u;
			} else if constexpr (RVIS64BIT(cpu)) {
				dst = mulhu64(src1, src2);
			} else {
				dst = 0;
			}
			return;
		case 0x14: // DIV
			dst = rv_div<RVREGTYPE(cpu)>(src1, src2);
			return;
		case 0x15: // DIVU
			if (LIKELY(src2 != 0)) {
				dst = src1 / src2;
			} else {
				dst = (RVREGTYPE(cpu)) -1;
			}
			return;
		case 0x16: // REM
			dst = rv_rem<RVREGTYPE(cpu)>(src1, src2);
			return;
		case 0x17: // REMU
			if (LIKELY(src2 != 0)) {
				dst = src1 % src2;
			} else {
				dst = src1;
			}
			return;
		case 0x44: { // PACK (ZEXT.H on RV32 when rs2 == 0)
			constexpr auto half = RVXLEN(cpu) / 2;
			const RVREGTYPE(cpu) mask = (RVREGTYPE(cpu)(1) << half) - 1;
			dst = (src1 & mask) | ((src2 & mask) << half);
			} return;
		case 0x47: // PACKH
			dst = RVREGTYPE(cpu)(uint8_t(src1))
				| (RVREGTYPE(cpu)(uint8_t(src2)) << 8);
			return;
		case 0x51: { // CLMUL
			RVREGTYPE(cpu) result = 0;
			for (unsigned i = 0; i < RVXLEN(cpu); i++)
				if ((src2 >> i) & 1)
					result ^= (src1 << i);
			dst = result;
			} return;
		case 0x52: { // CLMULR
			RVREGTYPE(cpu) result = 0;
			for (unsigned i = 0; i < RVXLEN(cpu); i++)
				if ((src2 >> i) & 1)
					result ^= (src1 >> (RVXLEN(cpu) - i - 1));
			dst = result;
			} return;
		case 0x53: { // CLMULH
			RVREGTYPE(cpu) result = 0;
			for (unsigned i = 1; i < RVXLEN(cpu); i++)
				if ((src2 >> i) & 1)
					result ^= (src1 >> (RVXLEN(cpu) - i));
			dst = result;
			} return;
		case 0x54: // MIN
			dst = (RVSIGNTYPE(cpu)(src1) < RVSIGNTYPE(cpu)(src2)) ? src1 : src2;
			return;
		case 0x55: // MINU
			dst = (src1 < src2) ? src1 : src2;
			return;
		case 0x56: // MAX
			dst = (RVSIGNTYPE(cpu)(src1) > RVSIGNTYPE(cpu)(src2)) ? src1 : src2;
			return;
		case 0x57: // MAXU
			dst = (src1 > src2) ? src1 : src2;
			return;
		case 0x75: // CZERO.EQZ
			dst = (src2 == 0) ? 0 : src1;
			return;
		case 0x77: // CZERO.NEZ
			dst = (src2 != 0) ? 0 : src1;
			return;
		case 0x102: // SH1ADD
			dst = src2 + (src1 << 1);
			return;
		case 0x104: // SH2ADD
			dst = src2 + (src1 << 2);
			return;
		case 0x106: // SH3ADD
			dst = src2 + (src1 << 3);
			return;
		case 0x141: // BSET
			dst = src1 | (RVREGTYPE(cpu)(1) << (src2 & (RVXLEN(cpu)-1)));
			return;
		case 0x204: // XNOR
			dst = ~(src1 ^ src2);
			return;
		case 0x205: // SRA
			dst = (RVSIGNTYPE(cpu))src1 >> (src2 & (RVXLEN(cpu)-1));
			return;
		case 0x206: // ORN
			dst = src1 | ~src2;
			return;
		case 0x207: // ANDN
			dst = src1 & ~src2;
			return;
		case 0x241: // BCLR
			dst = src1 & ~(RVREGTYPE(cpu)(1) << (src2 & (RVXLEN(cpu)-1)));
			return;
		case 0x245: // BEXT
			dst = (src1 >> (src2 & (RVXLEN(cpu)-1))) & 1;
			return;
		// The complementary shift count is masked, as a zero rotate would
		// otherwise shift by the full register width
		case 0x301: { // ROL: Rotate left
			const auto shift = src2 & (RVXLEN(cpu) - 1);
			dst = (src1 << shift) | (src1 >> ((RVXLEN(cpu) - shift) & (RVXLEN(cpu) - 1)));
			} return;
		case 0x305: { // ROR: Rotate right
			const auto shift = src2 & (RVXLEN(cpu) - 1);
			dst = (src1 >> shift) | (src1 << ((RVXLEN(cpu) - shift) & (RVXLEN(cpu) - 1)));
			} return;
		case 0x341: // BINV
			dst = src1 ^ (RVREGTYPE(cpu)(1) << (src2 & (RVXLEN(cpu)-1)));
			return;
		}
		cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION, instr.whole);
	},
	[] (char* buffer, size_t len, auto& cpu, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVDISASM::op_reg(buffer, len, instr, RVISGE64BIT(cpu));
	});

	INSTRUCTION(SYSTEM,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_COLDATTR {
		cpu.machine().system(instr);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVDISASM::op_system(buffer, len, instr);
	});

	INSTRUCTION(OP_ADD,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		auto& dst = cpu.reg(instr.Rtype.rd);
		dst = cpu.reg(instr.Rtype.rs1) + cpu.reg(instr.Rtype.rs2);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVDISASM::rrr(buffer, len, "add", instr);
	});

	INSTRUCTION(OP_SUB,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		auto& dst = cpu.reg(instr.Rtype.rd);
		dst = cpu.reg(instr.Rtype.rs1) - cpu.reg(instr.Rtype.rs2);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVDISASM::rrr(buffer, len, "sub", instr);
	});

	INSTRUCTION(SYSCALL,
	[] (auto& cpu, rv32i_instruction) RVINSTR_ATTR {
		cpu.machine().system_call(cpu.reg(REG_ECALL));
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVDISASM::op_system(buffer, len, instr);
	});

	INSTRUCTION(WFI,
	[] (auto& cpu, rv32i_instruction) RVINSTR_ATTR {
		cpu.machine().stop();
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVDISASM::op_system(buffer, len, instr);
	});

	INSTRUCTION(LUI,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		cpu.reg(instr.Utype.rd) = instr.Utype.upper_imm();
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVDISASM::op_lui(buffer, len, instr);
	});

	INSTRUCTION(AUIPC,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		cpu.reg(instr.Utype.rd) = cpu.pc() + instr.Utype.upper_imm();
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVDISASM::op_auipc(buffer, len, instr);
	});

	INSTRUCTION(OP_IMM32_ADDIW,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		auto& dst = cpu.reg(instr.Itype.rd);
		const uint32_t src = cpu.reg(instr.Itype.rs1);
		// ADDIW: Add 32-bit sign-extended 12-bit immediate
		dst = (int32_t) (src + RVIMM(cpu, instr.Itype));
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVDISASM::rri(buffer, len, "addiw", instr);
	});

	INSTRUCTION(OP_IMM32_SLLIW,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		auto& dst = cpu.reg(instr.Itype.rd);
		const uint32_t src = cpu.reg(instr.Itype.rs1);
		// SLLIW: Shift-Left Logical 0-31 immediate
		dst = (int32_t) (src << instr.Itype.shift_imm());
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVDISASM::rrs(buffer, len, "slliw", instr, instr.Itype.imm & 0x1F);
	});

	INSTRUCTION(OP_IMM32_SRLIW,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		auto& dst = cpu.reg(instr.Itype.rd);
		const uint32_t src = cpu.reg(instr.Itype.rs1);
		// SRLIW: Shift-Right Logical 0-31 immediate
		dst = (int32_t) (src >> instr.Itype.shift_imm());
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVDISASM::rrs(buffer, len, "srliw", instr, instr.Itype.imm & 0x1F);
	});

	INSTRUCTION(OP_IMM32_SRAIW,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		auto& dst = cpu.reg(instr.Itype.rd);
		const uint32_t src = cpu.reg(instr.Itype.rs1);
		// SRAIW: Arithmetic right shift, preserve the sign bit
		dst = (int32_t)src >> instr.Itype.shift_imm();
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVDISASM::rrs(buffer, len, "sraiw", instr, instr.Itype.imm & 0x1F);
	});


	INSTRUCTION(OP_IMM32_SLLI_UW,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		auto& dst = cpu.reg(instr.Itype.rd);
		const uint32_t src = cpu.reg(instr.Itype.rs1);
		// SLLI.UW: Shift-left Unsigned Word (Immediate)
		// The shift amount is a full 6-bit RV64 shamt, not the 5-bit *W shamt
		dst = RVREGTYPE(cpu)(src) << instr.Itype.shift64_imm();
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVDISASM::rrs(buffer, len, "slli.uw", instr, instr.Itype.imm & 0x3F);
	});

	INSTRUCTION(OP_IMM32,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		auto& dst = cpu.reg(instr.Itype.rd);
		const uint32_t src = cpu.reg(instr.Itype.rs1);

		switch (instr.Itype.funct3) {
		case 0x1:
			switch (instr.Itype.imm) {
			case 0b011000000000: // CLZ.W
#ifdef RISCV_HAS_BITOPS
				dst = std::countl_zero(src);
#else
				dst = src ? __builtin_clz(src) : RVXLEN(cpu);
#endif
				return;
			case 0b011000000001: // CTZ.W
#ifdef RISCV_HAS_BITOPS
				dst = std::countr_zero(src);
#else
				dst = src ? __builtin_ctz(src) : 0;
#endif
				return;
			case 0b011000000010: // CPOP.W
#ifdef RISCV_HAS_BITOPS
				dst = std::popcount(src);
#else
				dst = __builtin_popcount(src);
#endif
				return;
			}
			break;
		case 0x5:
			if (instr.Itype.high_bits() == 0x600 && (instr.Itype.imm & 0x20) == 0) // RORIW
			{
				const auto shift = instr.Itype.imm & 31;
				dst = (int32_t) ((src >> shift) | (src << ((32 - shift) & 31)));
				return;
			}
			break;
		}
		cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION, instr.whole);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVDISASM::op_imm32(buffer, len, instr);
	});

	INSTRUCTION(OP32,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		auto& dst = cpu.reg(instr.Rtype.rd);
		const uint32_t src1 = cpu.reg(instr.Rtype.rs1);
		const uint32_t src2 = cpu.reg(instr.Rtype.rs2);

		switch (instr.Rtype.jumptable_friendly_op()) {
		case 0x1: // SLLW
			dst = (int32_t) ((uint32_t)src1 << (src2 & 31));
			return;
		case 0x5: // SRLW: Logical right shift 32-bit
			dst = (int32_t) ((uint32_t)src1 >> (src2 & 31));
			return;
		// M-extension
		case 0x10: // MULW (signed 32-bit multiply, sign-extended)
			dst = (int32_t) (src1 * src2);
			return;
		case 0x14: // DIVW
			dst = (int32_t) rv_div<uint32_t>(src1, src2);
			return;
		case 0x15: // DIVUW
			if (LIKELY(src2 != 0)) {
				dst = (int32_t) (src1 / src2);
			} else {
				dst = (RVREGTYPE(cpu)) -1;
			}
			return;
		case 0x16: // REMW
			dst = (int32_t) rv_rem<uint32_t>(src1, src2);
			return;
		case 0x17: // REMUW
			if (LIKELY(src2 != 0)) {
				dst = (int32_t) (src1 % src2);
			} else {
				dst = int32_t(src1);
			}
			return;
		case 0x40: // ADD.UW
			dst = cpu.reg(instr.Rtype.rs2) + RVREGTYPE(cpu)(src1);
			return;
		case 0x44: // ZEXT.H / PACKW
			if (instr.Rtype.rs2 == 0) {
				dst = uint16_t(src1);
			} else {
				dst = int32_t(uint16_t(src1) | (uint32_t(uint16_t(src2)) << 16));
			}
			return;
		case 0x102: // SH1ADD.UW
			dst = cpu.reg(instr.Rtype.rs2) + (RVREGTYPE(cpu)(src1) << 1);
			return;
		case 0x104: // SH2ADD.UW
			dst = cpu.reg(instr.Rtype.rs2) + (RVREGTYPE(cpu)(src1) << 2);
			return;
		case 0x106: // SH3ADD.UW
			dst = cpu.reg(instr.Rtype.rs2) + (RVREGTYPE(cpu)(src1) << 3);
			return;
		case 0x200: // SUBW
			dst = (int32_t) (src1 - src2);
			return;
		case 0x205: // SRAW
			dst = (int32_t)src1 >> (src2 & 31);
			return;
		case 0x301: {
			// ROLW: Rotate left 32-bit
			const auto shift = src2 & 31;
			dst = (int32_t) ((src1 << shift) | (src1 >> ((32 - shift) & 31)));
			} return;
		case 0x305: {
			// RORW: Rotate right 32-bit
			const auto shift = src2 & 31;
			dst = (int32_t) ((src1 >> shift) | (src1 << ((32 - shift) & 31)));
			} return;
		}
		cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION, instr.whole);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVDISASM::op_reg32(buffer, len, instr);
	});

	INSTRUCTION(OP32_ADDW,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
		auto& dst = cpu.reg(instr.Rtype.rd);
		const uint32_t src1 = cpu.reg(instr.Rtype.rs1);
		const uint32_t src2 = cpu.reg(instr.Rtype.rs2);
		dst = (int32_t) (src1 + src2);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVDISASM::rrr(buffer, len, "addw", instr);
	});

	INSTRUCTION(FENCE,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_COLDATTR {
		if (instr.Itype.funct3 == 0x1)
			cpu.machine().memory.mark_execute_segments_stale();
		// Do a full barrier, for now
		std::atomic_thread_fence(std::memory_order_seq_cst);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVDISASM::op_misc_mem(buffer, len, instr);
	});

	INSTRUCTION(CBO,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_COLDATTR {
		switch (instr.Itype.imm) {
		case RV32I_CBO_INVAL:
		case RV32I_CBO_CLEAN:
		case RV32I_CBO_FLUSH:
			return;
		case RV32I_CBO_ZERO: {
			const auto addr = cpu.reg(instr.Itype.rs1)
				& ~RVREGTYPE(cpu)(RV32I_CBO_BLOCK - 1);
			auto& memory = cpu.machine().memory;
			for (unsigned i = 0; i < RV32I_CBO_BLOCK; i += 8)
				memory.template write<uint64_t> (addr + i, 0);
			return;
		}
		default:
			cpu.trigger_exception(ILLEGAL_OPCODE, instr.whole);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		return RVDISASM::op_misc_mem(buffer, len, instr);
	});
}
