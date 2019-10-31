#include "rv32i.hpp"
#include "machine.hpp"
#include "rv32i_instr.cpp"
#include "rv32a_instr.cpp"
#include "rv32c_instr.cpp"

namespace riscv
{
	template<>
	const CPU<4>::instruction_t& CPU<4>::decode(const format_t instruction) const
	{
		// if all bits are zero, it's an illegal instruction (by design)
		if (UNLIKELY(instruction.whole == 0x0)) {
			return DECODED_INSTR(ILLEGAL);
		}
		else if (instruction.length() == 2)
		{
			// RV32 C
			const auto ci = instruction.compressed();
			switch (ci.opcode())
			{
				// Quadrant 0
				case CI_CODE(0b000, 0b00):
					return DECODED_COMPR(C0_ADDI4SPN);
				case CI_CODE(0b001, 0b00):
				case CI_CODE(0b010, 0b00):
				case CI_CODE(0b011, 0b00):
					return DECODED_COMPR(C0_REG_LOAD);
				// RESERVED: 0b100, 0b00
				case CI_CODE(0b101, 0b00):
				case CI_CODE(0b110, 0b00):
				case CI_CODE(0b111, 0b00):
					return DECODED_COMPR(C0_REG_STORE);
				// Quadrant 1
				case CI_CODE(0b000, 0b01):
					return DECODED_COMPR(C1_NOP_ADDI);
				case CI_CODE(0b001, 0b01):
					return DECODED_COMPR(C1_JAL);
				case CI_CODE(0b010, 0b01):
					return DECODED_COMPR(C1_LI);
				case CI_CODE(0b011, 0b01):
					return DECODED_COMPR(C1_ADDI16SP_LUI);
				case CI_CODE(0b100, 0b01):
					return DECODED_COMPR(C1_ALU_OPS);
				case CI_CODE(0b101, 0b01):
					return DECODED_COMPR(C1_JUMP);
				case CI_CODE(0b110, 0b01):
					return DECODED_COMPR(C1_BEQZ);
				case CI_CODE(0b111, 0b01):
					return DECODED_COMPR(C1_BNEZ);
				// Quadrant 2
				case CI_CODE(0b000, 0b10):
				case CI_CODE(0b001, 0b10):
				case CI_CODE(0b010, 0b10):
				case CI_CODE(0b011, 0b10):
					return DECODED_COMPR(C2_SP_LOAD);
				case CI_CODE(0b100, 0b10):
					if (UNLIKELY(ci.whole == 0b1001000000000010)) {
						return DECODED_COMPR(C2_EBREAK);
					}
					return DECODED_COMPR(C2_VARIOUS);
				case CI_CODE(0b101, 0b10):
				case CI_CODE(0b110, 0b10):
				case CI_CODE(0b111, 0b10):
					return DECODED_COMPR(C2_SP_STORE);
			}
		}
		else // RV32 IMAF
		{
			switch (instruction.opcode())
			{
				// RV32IM
				case 0b0000011:
					return DECODED_INSTR(LOAD);
				case 0b0100011:
					return DECODED_INSTR(STORE);
				case 0b1000011:
					return DECODED_INSTR(MADD);
				case 0b1100011:
					return DECODED_INSTR(BRANCH);
				case 0b1100111:
					return DECODED_INSTR(JALR);
				case 0b1101111:
					return DECODED_INSTR(JAL);
				case 0b0010011:
					return DECODED_INSTR(OP_IMM);
				case 0b0110011:
					return DECODED_INSTR(OP);
				case 0b1110011:
					return DECODED_INSTR(SYSTEM);
				case 0b0110111:
					return DECODED_INSTR(LUI);
				case 0b0010111:
					return DECODED_INSTR(AUIPC);
				case 0b0011011:
					return DECODED_INSTR(OP_IMM32);
				case 0b0111011:
					return DECODED_INSTR(OP32);
				case 0b0001111:
					return DECODED_INSTR(FENCE);
				// RV32A - Atomic instructions
				case 0b0101111:
					switch (instruction.Atype.funct5)
					{
						case 0b00010:
							return DECODED_ATOMIC(LOAD_RESV);
						case 0b00011:
							return DECODED_ATOMIC(STORE_COND);
						case 0b00001:
							return DECODED_ATOMIC(AMOSWAP_W);
					}
			}
		}
		// illegal operation exception
		return DECODED_INSTR(UNIMPLEMENTED);
	}

	std::string RV32I::to_string(CPU<4>& cpu, format_t format, const instruction_t& instr)
	{
		char buffer[256];
		char ibuffer[128];
		int  ibuflen = instr.printer(ibuffer, sizeof(ibuffer), cpu, format);
		int  len = 0;
		if (format.length() == 4) {
			len = snprintf(buffer, sizeof(buffer),
					"[%08X] %08X %.*s",
					cpu.pc(), format.whole, ibuflen, ibuffer);
		}
		else if (format.length() == 2) {
			len = snprintf(buffer, sizeof(buffer),
					"[%08X]     %04hX %.*s",
					cpu.pc(), (uint16_t) format.whole, ibuflen, ibuffer);
		}
		else {
			throw std::runtime_error("Unimplemented instruction format length");
		}
		return std::string(buffer, len);
	}
}
