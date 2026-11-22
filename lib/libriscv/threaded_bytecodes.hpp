#pragma once
#include "libriscv_settings.h"

namespace riscv
{
	// Fused pairs (see fused_pairs.inc). A pair exists when the bytecodes of
	// both halves do: RISCV_FUSED_IF_<group>(x) expands to x or to nothing.
#define RISCV_FUSED_IF_COMMON(...) __VA_ARGS__
#ifdef RISCV_64I
#define RISCV_FUSED_IF_RV64(...) __VA_ARGS__
#else
#define RISCV_FUSED_IF_RV64(...) /* */
#endif
#ifdef RISCV_EXT_COMPRESSED
#define RISCV_FUSED_IF_C(...) __VA_ARGS__
#define RISCV_FUSED_IF_C_RV32(...) __VA_ARGS__
#define RISCV_FUSED_IF_C_RV64(...) RISCV_FUSED_IF_RV64(__VA_ARGS__)
#else
#define RISCV_FUSED_IF_C(...) /* */
#define RISCV_FUSED_IF_C_RV32(...) /* */
#define RISCV_FUSED_IF_C_RV64(...) /* */
#endif

	// Bytecodes for threaded simulation
	enum
	{
		RV32I_BC_INVALID = 0,
		RV32I_BC_ADDI,
		RV32I_BC_LI,
		RV32I_BC_MV,

		RV32I_BC_SLLI,
		RV32I_BC_SLTI,
		RV32I_BC_SLTIU,
		RV32I_BC_XORI,
		RV32I_BC_SRLI,
		RV32I_BC_SRAI,
		RV32I_BC_ORI,
		RV32I_BC_ANDI,

		RV32I_BC_LUI,
		RV32I_BC_AUIPC,

		RV32I_BC_LDB,
		RV32I_BC_LDBU,
		RV32I_BC_LDH,
		RV32I_BC_LDHU,
		RV32I_BC_LDW,

		RV32I_BC_STB,
		RV32I_BC_STH,
		RV32I_BC_STW,

#ifdef RISCV_64I
		RV32I_BC_LDWU,
		RV32I_BC_LDD,
		RV32I_BC_STD,
#endif

		RV32I_BC_BEQ,
		RV32I_BC_BNE,
		RV32I_BC_BLT,
		RV32I_BC_BGE,
		RV32I_BC_BLTU,
		RV32I_BC_BGEU,
		RV32I_BC_BEQ_FW,
		RV32I_BC_BNE_FW,

		RV32I_BC_JAL,
		RV32I_BC_JALR,
		RV32I_BC_FAST_JAL,
		RV32I_BC_FAST_CALL,

		RV32I_BC_OP_ADD,
		RV32I_BC_OP_SUB,
		RV32I_BC_OP_SLL,
		RV32I_BC_OP_SLT,
		RV32I_BC_OP_SLTU,
		RV32I_BC_OP_XOR,
		RV32I_BC_OP_SRL,
		RV32I_BC_OP_OR,
		RV32I_BC_OP_AND,
		RV32I_BC_OP_MUL,
		RV32I_BC_OP_DIV,
		RV32I_BC_OP_DIVU,
		RV32I_BC_OP_REM,
		RV32I_BC_OP_REMU,
		RV32I_BC_OP_SRA,
		RV32I_BC_OP_ZEXT_H,
		RV32I_BC_OP_SH1ADD,
		RV32I_BC_OP_SH2ADD,
		RV32I_BC_OP_SH3ADD,

		RV32I_BC_SEXT_B,
		RV32I_BC_SEXT_H,
		RV32I_BC_BSETI,
		RV32I_BC_BEXTI,

#ifdef RISCV_64I
		RV64I_BC_ADDIW,
		RV64I_BC_SLLIW,
		RV64I_BC_SRLIW,
		RV64I_BC_SRAIW,
		RV64I_BC_OP_ADDW,
		RV64I_BC_OP_SUBW,
		RV64I_BC_OP_MULW,
		RV64I_BC_OP_ADD_UW,
		RV64I_BC_OP_SH1ADD_UW,
		RV64I_BC_OP_SH2ADD_UW,
#endif

#ifdef RISCV_EXT_COMPRESSED
		RV32C_BC_ADDI,
		RV32C_BC_LI,
		RV32C_BC_MV,
		RV32C_BC_SLLI,
		RV32C_BC_BEQZ,
		RV32C_BC_BNEZ,
		RV32C_BC_JMP,
		RV32C_BC_JR,
		RV32C_BC_JAL_ADDIW,
		RV32C_BC_JALR,
		RV32C_BC_LDD,
		RV32C_BC_STD,
		RV32C_BC_LDW,
		RV32C_BC_STW,
		RV32C_BC_SRLI,
		RV32C_BC_ANDI,
		RV32C_BC_ADD,
		RV32C_BC_XOR,
		RV32C_BC_OR,
		RV32C_BC_LUI,
		RV32C_BC_FLD,
		RV32C_BC_FSD,
		RV32C_BC_FLW,
		RV32C_BC_FSW,
		RV32C_BC_SRAI,
		RV32C_BC_SUB,
		RV32C_BC_AND,
		RV32C_BC_SUBW,
		RV32C_BC_ADDW,
		RV32C_BC_FUNCTION,
#endif

		RV32I_BC_SYSCALL,
		RV32I_BC_DYNCALL,
		RV32I_BC_STOP,

		RV32F_BC_FLW,
		RV32F_BC_FLD,
		RV32F_BC_FSW,
		RV32F_BC_FSD,
		RV32F_BC_FADD,
		RV32F_BC_FSUB,
		RV32F_BC_FMUL,
		RV32F_BC_FDIV,
		RV32F_BC_FMADD,
		RV32I_BC_FUNCTION,
		RV32I_BC_FUNCBLOCK,
#ifdef RISCV_BINARY_TRANSLATION
		RV32I_BC_TRANSLATOR,
#endif
#ifdef RISCV_ASMJIT
		RV32I_BC_ASMJIT,
#endif
		RV32I_BC_LIVEPATCH,
		RV32I_BC_SYSTEM,
#define FUSED_PAIR(name, a, b, A, B, G) RISCV_FUSED_IF_ ## G(RV32I_BC_F_ ## name,)
#include "fused_pairs.inc"
#undef FUSED_PAIR
		BYTECODES_MAX
	};
	static_assert(BYTECODES_MAX <= 256, "A bytecode must fit in a byte");

	// The first (A) and second (B) bytecode of each fused pair, and the
	// register width it is for (0 = both): some compressed bytecodes are
	// different instructions in RV32 and RV64
	struct FusedPair { unsigned char first, second, width; };
#define RISCV_FUSED_WIDTH_COMMON 0
#define RISCV_FUSED_WIDTH_C 0
#define RISCV_FUSED_WIDTH_RV64 8
#define RISCV_FUSED_WIDTH_C_RV64 8
#define RISCV_FUSED_WIDTH_C_RV32 4
#define FUSED_PAIR(name, a, b, A, B, G) RISCV_FUSED_IF_ ## G(FusedPair{A, B, RISCV_FUSED_WIDTH_ ## G},)
	inline constexpr FusedPair fused_pairs[] = {
#include "fused_pairs.inc"
	};
#undef FUSED_PAIR
#undef RISCV_FUSED_WIDTH_COMMON
#undef RISCV_FUSED_WIDTH_C
#undef RISCV_FUSED_WIDTH_RV64
#undef RISCV_FUSED_WIDTH_C_RV64
#undef RISCV_FUSED_WIDTH_C_RV32
	static constexpr unsigned RV32I_BC_FUSED_COUNT = sizeof(fused_pairs) / sizeof(fused_pairs[0]);
	static constexpr unsigned RV32I_BC_FUSED_FIRST = BYTECODES_MAX - RV32I_BC_FUSED_COUNT;
#ifdef RISCV_EXT_COMPRESSED
	// The compressed bytecodes (2-byte instructions)
	static constexpr unsigned RV32C_BC_FIRST = RV32C_BC_ADDI;
	static constexpr unsigned RV32C_BC_LAST  = RV32C_BC_FUNCTION;
#else
	static constexpr unsigned RV32C_BC_FIRST = 256;
	static constexpr unsigned RV32C_BC_LAST  = 256;
#endif

	union FasterItype
	{
		uint32_t whole;

		struct
		{
			uint16_t imm;
			uint8_t  rs2;
			uint8_t  rs1;
		};

		RISCV_ALWAYS_INLINE
		auto get_rs1() const noexcept {
			return rs1;
		}
		RISCV_ALWAYS_INLINE
		auto get_rs2() const noexcept {
			return rs2;
		}
		RISCV_ALWAYS_INLINE
		int32_t signed_imm() const noexcept {
			return (int16_t)imm;
		}
		RISCV_ALWAYS_INLINE
		auto unsigned_imm() const noexcept {
			return (uint16_t)imm;
		}
	};

	union FasterOpType
	{
		uint32_t whole;

		struct
		{
			uint16_t rd;
			uint8_t rs2;
			uint8_t rs1;
		};

		RISCV_ALWAYS_INLINE
		auto get_rd() const noexcept {
			return rd;
		}
		RISCV_ALWAYS_INLINE
		auto get_rs1() const noexcept {
			return rs1;
		}
		RISCV_ALWAYS_INLINE
		auto get_rs2() const noexcept {
			return rs2;
		}
	};

	union FasterImmediate
	{
		uint32_t whole;

		struct
		{
			uint8_t  rd;
			int16_t  imm;
		};

		RISCV_ALWAYS_INLINE
		auto get_rd() const noexcept {
			return rd;
		}

		RISCV_ALWAYS_INLINE
		int32_t signed_imm() const noexcept {
			return imm;
		}
	};

	union FasterMove
	{
		uint32_t whole;

		struct
		{
			uint8_t rs1;
			uint8_t rd;
		};

		RISCV_ALWAYS_INLINE
		auto get_rd() const noexcept {
			return rd;
		}
		RISCV_ALWAYS_INLINE
		auto get_rs1() const noexcept {
			return rs1;
		}
	};

	union FasterJtype
	{
		uint32_t whole;

		struct
		{
			uint32_t offset : 24;
			uint32_t rd     : 8;
		};

		int32_t signed_imm() const noexcept {
			return int32_t(offset << (32 - 24)) >> (32 - 24);
		}
		int32_t upper_imm() const noexcept {
			return offset << 8;
		}
	};

	union FasterFloatType
	{
		uint32_t whole;

		struct
		{
			uint8_t func;
			uint8_t rd;
			uint8_t rs2;
			uint8_t rs1;
		};

		RISCV_ALWAYS_INLINE
		auto get_rd() const noexcept {
			return rd;
		}
		RISCV_ALWAYS_INLINE
		auto get_rs1() const noexcept {
			return rs1;
		}
		RISCV_ALWAYS_INLINE
		auto get_rs2() const noexcept {
			return rs2;
		}
	};

} // riscv
