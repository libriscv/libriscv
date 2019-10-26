#pragma once
#include <array>
#include <cstdint>
#include <string>
#include "types.hpp"

namespace riscv
{
	union rv32c_instruction
	{
		using word_t = uint32_t;
		using sword_t = int32_t;
		static constexpr int REG_OFFSET = 0x8;

		// register format
		struct {
			uint16_t opcode : 2;
			uint16_t rs2    : 5;
			uint16_t rd     : 5;
			uint16_t funct4 : 4;
		} CR;
		// immediate format
		struct {
			uint16_t opcode : 2;
			uint16_t imm1   : 5;
			uint16_t rd     : 5;
			uint16_t imm2   : 1;
			uint16_t funct3 : 3;

			bool sign() const noexcept {
				return imm2;
			}
			int32_t signed_imm() const noexcept {
				const uint32_t ext = 0xFFFFFFE0;
				return imm1 | (sign() ? ext : 0);
			}
		} CI;
		// stack-relative store
		struct {
			uint16_t opcode : 2;
			uint16_t rs2    : 5;
			uint16_t imm    : 6;
			uint16_t funct3 : 3;
		} CSS;
		// wide immediate format
		struct {
			uint16_t opcode : 2;
			uint16_t srd    : 3;
			uint16_t imm    : 8;
			uint16_t funct3 : 3;
		} CIW;
		// load format
		struct {
			uint16_t opcode : 2;
			uint16_t srs2   : 3;
			uint16_t imm1   : 2;
			uint16_t srs1   : 3;
			uint16_t imm2   : 3;
			uint16_t funct3 : 3;
		} CL;
		// store format
		struct {
			uint16_t opcode : 2;
			uint16_t srs2   : 3;
			uint16_t imm1   : 2;
			uint16_t srs1   : 3;
			uint16_t imm2   : 3;
			uint16_t funct3 : 3;
		} CS;
		// arithmetic format
		struct {
			uint16_t opcode : 2;
			uint16_t srs2   : 3;
			uint16_t funct2 : 2;
			uint16_t srs1   : 3;
			uint16_t funct6 : 6;
		} CA;
		// branch format
		struct {
			uint16_t opcode : 2;
			uint16_t off1   : 5;
			uint16_t srs1   : 3;
			uint16_t off2   : 3;
			uint16_t funct3 : 3;
		} CB;
		// jump format
		struct {
			uint16_t opcode : 2;
			uint16_t jump   : 11;
			uint16_t funct3 : 3;
		} CJ;

		uint16_t whole;

		rv32c_instruction() : whole(0) {}
		rv32c_instruction(uint16_t another) : whole(another) {}

		uint16_t opcode() const noexcept {
			return whole & 0b1110000000000011;
		}
		uint16_t funct3() const noexcept {
			return whole >> 13;
		}
	};
	static_assert(sizeof(rv32c_instruction) == 2, "Instruction is 2 bytes");
}
