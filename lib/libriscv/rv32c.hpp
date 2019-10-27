#pragma once
#include <array>
#include <cstdint>
#include <string>
#include "types.hpp"

namespace riscv
{
	union rv32c_instruction
	{
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
		struct {
			uint16_t opcode : 2;
			uint16_t imm234 : 3;
			uint16_t imm5   : 1;
			uint16_t imm6   : 1;
			uint16_t rd     : 5;
			uint16_t imm7   : 1;
			uint16_t funct3 : 3;

			bool sign() const noexcept {
				return imm5;
			}
			int32_t signed_imm() const noexcept {
				const uint32_t ext = 0xFFFFFFE0;
				int32_t val = imm6 | (imm7 << 1) | (imm234 << 2);
				return (val << 2) | (sign() ? ext : 0);
			}
		} CI2;
		// stack-relative store
		struct {
			uint16_t opcode : 2;
			uint16_t rs2    : 5;
			uint16_t imm    : 6;
			uint16_t funct3 : 3;

			int32_t offset4() const noexcept {
				//return ((imm & 0xF) << 4) | ((imm & 0x30) >> 4);
				return imm;
			}
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
			uint16_t srd    : 3;
			uint16_t imm2   : 1;
			uint16_t imm3   : 1;
			uint16_t srs1   : 3;
			uint16_t imm45  : 2;
			uint16_t imm6   : 1;
			uint16_t funct3 : 3;

			bool sign() const noexcept {
				return imm6;
			}
			int32_t signed_imm() const noexcept {
				const uint32_t ext = 0xFFFFFFE0;
				int32_t val = imm6 | (imm2 << 1) | (imm3 << 2) | (imm45 << 3);
				return (val << 2) | (sign() ? ext : 0);
			}
		} CL;
		// store format
		struct {
			uint16_t opcode : 2;
			uint16_t srs2   : 3;
			uint16_t imm6   : 1;
			uint16_t imm2   : 1;
			uint16_t srs1   : 3;
			uint16_t imm3   : 3;
			uint16_t funct3 : 3;

			int32_t offset4() const noexcept {
				return (imm2 << 2) | (imm3 << 3) | (imm6 << 6);
			}
		} CS;
		// arithmetic format
		struct {
			uint16_t opcode : 2;
			uint16_t srs2   : 3;
			uint16_t funct2 : 2;
			uint16_t srd    : 3;
			uint16_t funct6 : 6;
		} CA;
		struct {
			uint16_t opcode : 2;
			uint16_t imm1   : 5;
			uint16_t srd    : 3;
			uint16_t funct2 : 2;
			uint16_t imm2   : 1;
			uint16_t funct3 : 3;

			int32_t shift_imm() const noexcept {
				return imm1;
			}
			int32_t signed_imm() const noexcept {
				const uint32_t ext = 0xFFFFFFE0;
				return imm1 | (imm2 ? ext : 0);
			}
		} CAB;
		// branch format
		struct {
			uint16_t opcode : 2;
			uint16_t off5   : 1;
			uint16_t off12  : 2;
			uint16_t off67  : 2;
			uint16_t srs1   : 3;
			uint16_t off34  : 2;
			uint16_t off8   : 1;
			uint16_t funct3 : 3;

			bool sign() const noexcept {
				return off8;
			}
			int32_t signed_imm() const noexcept {
				int32_t val = (off12 | (off34 << 2) | (off5 << 4) | (off67 << 5));
				const uint32_t ext = 0xFFFFFF00; // 7+1 immediate bits + 1 sign
				return (val << 1) | (sign() ? ext : 0);
			}
		} CB;
		// jump format
		struct {
			uint16_t opcode : 2;
			//uint16_t jump   : 11;
			uint16_t off5   : 1;
			uint16_t off13  : 3;
			uint16_t off7   : 1;
			uint16_t off6   : 1;
			uint16_t off10  : 1;
			uint16_t off89  : 2;
			uint16_t off4   : 1;
			uint16_t off11  : 1;
			uint16_t funct3 : 3;

			bool sign() const noexcept {
				return off11;
			}
			int32_t signed_imm() const noexcept {
				int32_t val = (off13 | (off4 << 3) | (off5 << 4) | (off6 << 5)
							| (off7 << 6) | (off89 << 7) | (off10 << 9));
				const uint32_t ext = 0xFFFFF800; // 12 immediate bits
				return (val << 1) | (sign() ? ext : 0);
			}
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
