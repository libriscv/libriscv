#pragma once
#include "rv32i_instr.hpp"

namespace riscv
{
	union rv32f_instruction
	{
		struct {
			uint32_t opcode : 7;
			uint32_t rd     : 5;
			uint32_t funct3 : 3;
			uint32_t rs1    : 5;
			uint32_t rs2    : 5;
			uint32_t funct7 : 7;
		} Rtype;
		struct {
			uint32_t opcode : 7;
			uint32_t rd     : 5;
			uint32_t funct3 : 3;
			uint32_t rs1    : 5;
			uint32_t rs2    : 5;
			uint32_t funct2 : 2;
			uint32_t rs3    : 5;
		} R4type;
		struct {
			uint32_t opcode : 7;
			uint32_t rd     : 5;
			uint32_t funct3 : 3;
			uint32_t rs1    : 5;
			uint32_t imm    : 11;
			uint32_t imms   : 1;

			bool sign() const noexcept {
				return imms;
			}
			int32_t signed_imm() const noexcept {
				const uint32_t ext = 0xFFFFF800;
				return (imm) | (sign() ? ext : 0);
			}
		} Itype;
		struct {
			uint32_t opcode : 7;
			uint32_t imm04  : 5;
			uint32_t funct3 : 3;
			uint32_t rs1    : 5;
			uint32_t rs2    : 5;
			uint32_t imm510 : 6;
			uint32_t imm11  : 1;

			bool sign() const noexcept {
				return imm11;
			}
			int32_t signed_imm() const noexcept {
				const uint32_t ext = 0xFFFFF800;
				const int32_t val = imm04 | (imm510 << 5);
				return val | (sign() ? ext : 0);
			}
		} Stype;

		uint16_t half[2];
		uint32_t whole;

		rv32f_instruction(rv32i_instruction i) : whole(i.whole) {}

		uint32_t opcode() const noexcept {
			return Rtype.opcode;
		}
	};
	static_assert(sizeof(rv32f_instruction) == 4, "Must be 4 bytes");

	enum fflags {
		FFLAG_NX = 0x1,
		FFLAG_UF = 0x2,
		FFLAG_OF = 0x4,
		FFLAG_DZ = 0x8,
		FFLAG_NV = 0x10
	};
}
