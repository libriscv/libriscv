#include "decoded_exec_segment.hpp"

#include "machine.hpp"
#include "threaded_bytecodes.hpp"
#include "instruction_list.hpp"
#include "rv32i_instr.hpp"
#include "rvc.hpp"
#include "rvfd.hpp"
#include "rvv.hpp"

namespace riscv
{
	template <int W> RISCV_INTERNAL
	size_t DecodedExecuteSegment<W>::threaded_rewrite(
		size_t bytecode, [[maybe_unused]] address_t pc, rv32i_instruction& instr)
	{
		static constexpr unsigned PCAL = compressed_enabled ? 2 : 4;
		const auto original = instr;

		switch (bytecode)
		{
			case RV32I_BC_MV: {
				FasterMove rewritten;
				rewritten.rd  = original.Itype.rd;
				rewritten.rs1 = original.Itype.rs1;

				instr.whole = rewritten.whole;
				return bytecode;
			}
			case RV32I_BC_LI: {
				FasterImmediate rewritten;
				rewritten.rd  = original.Itype.rd;
				rewritten.imm = original.Itype.signed_imm();

				instr.whole = rewritten.whole;
				return bytecode;
			}
			case RV64I_BC_ADDIW:
			case RV64I_BC_SRLIW:
			case RV32I_BC_ADDI:
			case RV32I_BC_SLLI:
			case RV32I_BC_SLTI:
			case RV32I_BC_SLTIU:
			case RV32I_BC_XORI:
			case RV32I_BC_SRLI:
			case RV32I_BC_SRAI:
			case RV32I_BC_ORI:
			case RV32I_BC_ANDI: {
				FasterItype rewritten;
				rewritten.rs1 = original.Itype.rd;
				rewritten.rs2 = original.Itype.rs1;
				rewritten.imm = original.Itype.signed_imm();

				instr.whole = rewritten.whole;
				return bytecode;
			}
			case RV32I_BC_BEQ:
			case RV32I_BC_BNE:
			case RV32I_BC_BLT:
			case RV32I_BC_BGE:
			case RV32I_BC_BLTU:
			case RV32I_BC_BGEU: {
				const int32_t imm = original.Btype.signed_imm();
				address_t addr = 0;
				const bool overflow = __builtin_add_overflow(pc, imm, &addr);

				if (!this->is_within(addr, 4) || (addr % PCAL) != 0 || overflow)
				{
					// Use invalid instruction for out-of-bounds branches
					// or misaligned jumps. It is strictly a cheat, but
					// it should also never happen on (especially) these
					// instructions. No sandbox harm.
					return RV32I_BC_INVALID;
				}

				FasterItype rewritten;
				rewritten.rs1 = original.Btype.rs1;
				rewritten.rs2 = original.Btype.rs2;
				rewritten.imm = original.Btype.signed_imm();

				instr.whole = rewritten.whole;

				// Forward branches can skip instr count check
				if (imm > 0 && bytecode == RV32I_BC_BEQ)
					return RV32I_BC_BEQ_FW;
				if (imm > 0 && bytecode == RV32I_BC_BNE)
					return RV32I_BC_BNE_FW;

				return bytecode;
			}
			case RV32I_BC_OP_ADD:
			case RV32I_BC_OP_SUB:
			case RV32I_BC_OP_SLL:
			case RV32I_BC_OP_SLT:
			case RV32I_BC_OP_SLTU:
			case RV32I_BC_OP_XOR:
			case RV32I_BC_OP_SRL:
			case RV32I_BC_OP_SRA:
			case RV32I_BC_OP_OR:
			case RV32I_BC_OP_AND:
			case RV32I_BC_OP_MUL:
			case RV32I_BC_OP_DIV:
			case RV32I_BC_OP_DIVU:
			case RV32I_BC_OP_REM:
			case RV32I_BC_OP_REMU:
			case RV32I_BC_OP_ZEXT_H:
			case RV32I_BC_OP_SH1ADD:
			case RV32I_BC_OP_SH2ADD:
			case RV32I_BC_OP_SH3ADD: {
				FasterOpType rewritten;
				rewritten.rd = original.Rtype.rd;
				rewritten.rs1 = original.Rtype.rs1;
				rewritten.rs2 = original.Rtype.rs2;

				instr.whole = rewritten.whole;
				return bytecode;
			}
			case RV32I_BC_LDB:
			case RV32I_BC_LDBU:
			case RV32I_BC_LDH:
			case RV32I_BC_LDHU:
			case RV32I_BC_LDW:
			case RV32I_BC_LDWU:
			case RV32I_BC_LDD: {
				FasterItype rewritten;
				rewritten.rs1 = original.Itype.rd;
				rewritten.rs2 = original.Itype.rs1;
				rewritten.imm = original.Itype.signed_imm();

				instr.whole = rewritten.whole;
				return bytecode;
			}
			case RV32I_BC_STB:
			case RV32I_BC_STH:
			case RV32I_BC_STW:
			case RV32I_BC_STD: {
				FasterItype rewritten;
				rewritten.rs1 = original.Stype.rs1;
				rewritten.rs2 = original.Stype.rs2;
				rewritten.imm = original.Stype.signed_imm();

				instr.whole = rewritten.whole;
				return bytecode;
			}
			case RV32F_BC_FLW:
			case RV32F_BC_FLD: {
				const rv32f_instruction fi{original};
				FasterItype rewritten;
				rewritten.rs1 = fi.Itype.rd;
				rewritten.rs2 = fi.Itype.rs1;
				rewritten.imm = fi.Itype.signed_imm();

				instr.whole = rewritten.whole;
				return bytecode;
			}
			case RV32F_BC_FSW:
			case RV32F_BC_FSD: {
				const rv32f_instruction fi{original};
				FasterItype rewritten;
				rewritten.rs1 = fi.Stype.rs1;
				rewritten.rs2 = fi.Stype.rs2;
				rewritten.imm = fi.Stype.signed_imm();

				instr.whole = rewritten.whole;
				return bytecode;
			}
			case RV32I_BC_JAL: {
				// Here we try to find out if the whole jump
				// can be expressed as just the instruction bits.
				const auto addr = pc + original.Jtype.jump_offset();
				const bool is_aligned = addr % PCAL == 0;
				const bool store_zero = original.Jtype.rd == 0;
				const bool store_ra = original.Jtype.rd == REG_RA;

				// The destination address also needs to be within
				// the current execute segment, as an optimization.
				if (this->is_within(addr, 4) && is_aligned)
				{
					const int32_t diff = addr - pc;
					if (!this->is_within(pc + diff, 4))
					{
						return RV32I_BC_INVALID;
					}
					else if (store_zero)
					{
						instr.whole = diff;
						return RV32I_BC_FAST_JAL;
					}
					else if (store_ra)
					{
						instr.whole = diff;
						return RV32I_BC_FAST_CALL;
					}
				}

				FasterJtype rewritten;
				rewritten.offset = original.Jtype.jump_offset();
				rewritten.rd     = original.Jtype.rd;

				instr.whole = rewritten.whole;
				return bytecode;
			}
			/** Vector instructions **/
#ifdef RISCV_EXT_VECTOR
			case RV32V_BC_VLE32:
			case RV32V_BC_VSE32: {
				const rv32v_instruction vi{instr};
				FasterItype rewritten;
				rewritten.rs1 = vi.VLS.vd;
				rewritten.rs2 = vi.VLS.rs1;
				rewritten.imm = 0;

				instr.whole = rewritten.whole;
				return bytecode;
			}
#endif
			/** Compressed instructions **/
#ifdef RISCV_EXT_COMPRESSED
			case RV32C_BC_ADDI: {
				const rv32c_instruction ci{original};

				FasterItype rewritten;
				if (ci.opcode() == RISCV_CI_CODE(0b000, 0b00))
				{
					// C.ADDI4SPN
					rewritten.rs1 = ci.CIW.srd + 8;
					rewritten.rs2 = REG_SP;
					rewritten.imm = ci.CIW.offset();
				}
				else if (ci.opcode() == RISCV_CI_CODE(0b011, 0b01))
				{
					// C.ADDI16SP
					rewritten.rs1 = REG_SP;
					rewritten.rs2 = REG_SP;
					rewritten.imm = ci.CI16.signed_imm();
				}
				else
				{	// C.ADDI
					rewritten.rs1 = ci.CI.rd;
					rewritten.rs2 = ci.CI.rd;
					rewritten.imm = ci.CI.signed_imm();
				}

				instr.whole = rewritten.whole;
				return RV32C_BC_ADDI;
			}
			case RV32C_BC_LI: {
				const rv32c_instruction ci{original};

				FasterItype rewritten;
				rewritten.rs1 = ci.CI.rd;
				rewritten.rs2 = 0;
				rewritten.imm = ci.CI.signed_imm();

				instr.whole = rewritten.whole;
				return RV32C_BC_ADDI;
			}
			case RV32C_BC_MV: {
				const rv32c_instruction ci{original};

				FasterMove rewritten;
				rewritten.rd  = ci.CR.rd;
				rewritten.rs1 = ci.CR.rs2;

				instr.whole = rewritten.whole;
				return RV32C_BC_MV;
			}
			case RV32C_BC_BNEZ: {
				const rv32c_instruction ci { original };

				const int32_t imm = ci.CB.signed_imm();
				const auto addr = pc + imm;

				if (!this->is_within(addr, 4) || (addr % PCAL) != 0)
				{
					return RV32I_BC_INVALID;
				}

				FasterItype rewritten;
				rewritten.rs1 = ci.CB.srs1 + 8;
				rewritten.rs2 = 0;
				rewritten.imm = imm;

				instr.whole = rewritten.whole;
				return RV32C_BC_BNEZ;
			}
			case RV32C_BC_LDD: {
				const rv32c_instruction ci{original};

				FasterItype rewritten;
				if ((ci.opcode() & 0x3) == 0x0)
				{	// C.LD
					rewritten.rs1 = ci.CSD.srs1 + 8;
					rewritten.rs2 = ci.CSD.srs2 + 8;
					rewritten.imm = ci.CSD.offset8();
				}
				else
				{	// C.LDSP
					rewritten.rs1 = ci.CIFLD.rd;
					rewritten.rs2 = REG_SP;
					rewritten.imm = ci.CIFLD.offset();
				}

				instr.whole = rewritten.whole;
				return bytecode;
			}
			case RV32C_BC_STD: {
				const rv32c_instruction ci{original};

				FasterItype rewritten;
				if ((ci.opcode() & 0x3) == 0x0)
				{	// C.SD
					rewritten.rs1 = ci.CSD.srs1 + 8;
					rewritten.rs2 = ci.CSD.srs2 + 8;
					rewritten.imm = ci.CSD.offset8();
				}
				else
				{	// C.SDSP
					rewritten.rs1 = REG_SP;
					rewritten.rs2 = ci.CSFSD.rs2;
					rewritten.imm = ci.CSFSD.offset();
				}

				instr.whole = rewritten.whole;
				return bytecode;
			}
#endif // RISCV_EXT_COMPRESSED
		}

		return bytecode;
	}

} // riscv
