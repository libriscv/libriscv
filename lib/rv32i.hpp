#pragma once
#include <array>
#include <cstdint>

namespace riscv
{
	union rv32i_register
	{
		uint32_t u32;
		int32_t  i32;
		uint16_t u16[2];
		int16_t  i16[2];
	};

	union rv32i_instruction
	{
		using word_t = uint32_t;

		// register-register
		struct {
			uint32_t funct7 : 7;
			uint32_t rs2    : 5;
			uint32_t rs1    : 5;
			uint32_t funct3 : 3;
			uint32_t rd     : 5;
			uint32_t opcode : 7;
		} Rtype;
		// register-immediate
		struct {
			uint32_t imm    : 12;
			uint32_t rs1    : 5;
			uint32_t funct3 : 3;
			uint32_t rd     : 5;
			uint32_t opcode : 7;
		} Itype;
		struct {
			uint32_t imm2   : 7;
			uint32_t rs2    : 5;
			uint32_t rs1    : 5;
			uint32_t funct3 : 3;
			uint32_t imm1   : 5;
			uint32_t opcode : 7;
		} Stype;
		struct {
			uint32_t imm    : 20;
			uint32_t rd     : 5;
			uint32_t opcode : 7;
		} Utype;
		// branch type
		struct {
			uint32_t funct7 : 7;
			uint32_t rs2    : 5;
			uint32_t rs1    : 5;
			uint32_t funct3 : 3;
			uint32_t rd     : 5;
			uint32_t opcode : 7;
		} Btype;
		struct {
			uint32_t imm4   : 1;
			uint32_t imm3   : 10;
			uint32_t imm2   : 1;
			uint32_t imm1   : 8;
			uint32_t rd     : 5;
			uint32_t opcode : 7;
		} Jtype;

		uint32_t whole;

		rv32i_instruction() : whole(0) {}
		rv32i_instruction(uint32_t another) : whole(another) {}

		uint16_t opcode() const noexcept {
			return Rtype.opcode;
		}

		uint16_t size() const noexcept {
			return 4;
		}

		bool sign() const noexcept {
			return whole & (1u << 31);
		}

		static rv32i_instruction from(uint32_t bits) noexcept {
			return { .whole = bits };
		}
	};
	static_assert(sizeof(rv32i_instruction) == 4, "Instruction is 4 bytes");

	struct RV32I {
		using address_t   = uint32_t; // ??
		using format_t    = rv32i_instruction;
		using register_t  = rv32i_register;
		static constexpr int INSTRUCTIONS = 40;
	};
}
