#include "rv32i.hpp"

namespace riscv
{
	union float_helper {
		float   fval;
		int32_t ival;
	};
	union double_helper {
		double  fval;
		int32_t ival[2];
	};

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
			uint32_t imm    : 12;
		} Itype;
		struct {
			uint32_t opcode : 7;
			uint32_t imm04  : 5;
			uint32_t funct3 : 3;
			uint32_t rs1    : 5;
			uint32_t rs2    : 5;
			uint32_t imm511 : 7;
		} Stype;

		uint16_t half[2];
		uint32_t whole;

		rv32f_instruction(rv32i_instruction i) : whole(i.whole) {}

		uint32_t opcode() const noexcept {
			return Rtype.opcode;
		}

	};

	enum fflags {
		FFLAG_NX = 0x1,
		FFLAG_UF = 0x2,
		FFLAG_OF = 0x4,
		FFLAG_DZ = 0x8,
		FFLAG_NV = 0x10
	};
}
