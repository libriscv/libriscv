#pragma once
#include <array>
#include <cstdint>

namespace riscv
{
	union rv32i_instruction
	{
		using word_t = uint32_t;

		// register format
		struct {
			uint32_t opcode : 7;
			uint32_t rd     : 5;
			uint32_t funct3 : 3;
			uint32_t rs1    : 5;
			uint32_t rs2    : 5;
			uint32_t funct7 : 7;
		} Rtype;
		// immediate format
		struct {
			uint32_t opcode : 7;
			uint32_t rd     : 5;
			uint32_t funct3 : 3;
			uint32_t rs1    : 5;
			uint32_t imm    : 12;

			bool sign() const noexcept {
				return imm & 2048;
			}
			int32_t signed_imm() const noexcept {
				const uint32_t ext = 0xFFFFF000;
				return imm | (sign() ? ext : 0);
			}
		} Itype;
		// store format
		struct {
			uint32_t opcode : 7;
			uint32_t imm1   : 5;
			uint32_t funct3 : 3;
			uint32_t rs1    : 5;
			uint32_t rs2    : 5;
			uint32_t imm2   : 7;

			bool sign() const noexcept {
				return imm2 & 0x40;
			}
			int32_t signed_imm() const noexcept {
				const uint32_t ext = 0xFFFFF000;
				return imm1 | (imm2 << 5) | (sign() ? ext : 0);
			}
		} Stype;
		// upper immediate format
		struct {
			uint32_t opcode : 7;
			uint32_t rd     : 5;
			uint32_t imm    : 20;

			bool sign() const noexcept {
				return imm & 0x80000;
			}
			int32_t signed_imm() const noexcept {
				const uint32_t ext = 0xFFF00000;
				return imm | (sign() ? ext : 0);
			}
		} Utype;
		// branch type
		struct {
			uint32_t opcode : 7;
			uint32_t rd     : 5;
			uint32_t funct3 : 3;
			uint32_t rs1    : 5;
			uint32_t rs2    : 5;
			uint32_t funct7 : 7;
		} Btype;
		// jump instructions
		struct {
			uint32_t opcode : 7;
			uint32_t rd     : 5;
			uint32_t imm1   : 8;
			uint32_t imm2   : 1;
			uint32_t imm3   : 10;
			uint32_t imm4   : 1;

			bool sign() const noexcept {
				return imm4;
			}
			int32_t jump_offset() const noexcept {
				const int32_t  jo  = (imm3 << 1) | (imm2 << 11) | (imm1 << 12);
				const uint32_t ext = 0xFFF00000;
				return jo | (sign() ? ext : 0);
			}
		} Jtype;

		uint32_t whole;

		rv32i_instruction() : whole(0) {}
		rv32i_instruction(uint32_t another) : whole(another) {}

		uint16_t opcode() const noexcept {
			return Rtype.opcode;
		}

		uint16_t length() const noexcept {
			return ((Rtype.opcode & 0x3) == 0x3) ? 4 : 2;
		}

		bool sign() const noexcept {
			return whole & (1u << 31);
		}
	};
	static_assert(sizeof(rv32i_instruction) == 4, "Instruction is 4 bytes");

	struct RV32I {
		using address_t   = uint32_t; // ??
		using format_t    = rv32i_instruction;
		using register_t  = uint32_t;
		static constexpr int INSTRUCTIONS = 40;
	};
}
