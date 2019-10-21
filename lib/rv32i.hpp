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
		} Itype;
		// store format
		struct {
			uint32_t opcode : 7;
			uint32_t imm1   : 5;
			uint32_t funct3 : 3;
			uint32_t rs1    : 5;
			uint32_t rs2    : 5;
			uint32_t imm2   : 7;
		} Stype;
		// upper immediate format
		struct {
			uint32_t opcode : 7;
			uint32_t rd     : 5;
			uint32_t imm    : 20;
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

		static const char* regname(const uint32_t reg) noexcept
		{
			switch (reg) {
				case 0: return "ZERO";
				case 1: return "RA";
				case 2: return "SP";
				case 3: return "GP";
				case 4: return "TP";
				case 5: return "LR";
				case 6: return "TMP0";
				case 7: return "TMP1";
				case 8: return "SFP";
				case 9: return "SFR";
				case 10: return "A0";
				case 11: return "A1";
				case 12: return "A2";
				case 13: return "A3";
				case 14: return "A4";
				case 15: return "A5";
				case 16: return "A6";
				case 17: return "A7";
				case 18: return "SR0";
				case 19: return "SR1";
				case 20: return "SR2";
				case 21: return "SR3";
				case 22: return "SR4";
				case 23: return "SR5";
				case 24: return "SR6";
				case 25: return "SR7";
				case 26: return "SR8";
				case 27: return "SR9";
				case 28: return "TMP2";
				case 29: return "TMP3";
				case 30: return "TMP4";
				case 31: return "TMP5";
			}
			return "Invalid register";
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
