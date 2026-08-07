#pragma once
#include "../rvc.hpp"
#include "../safe_instr_loader.hpp"
#include "../types.hpp"

namespace riscv
{
	/// @brief One guest instruction, normalized into 32-bit RV32I form.
	/// @details Compressed instructions are expanded to the equivalent base
	/// instruction, so that everything downstream of aj_decode() only ever deals
	/// with 32-bit encodings. `length` remains the *guest* length, because it is
	/// what PC arithmetic (link addresses, fall-through) must use.
	struct AjDecoded
	{
		rv32i_instruction instr {};
		uint8_t  length = 0;
		bool     valid  = false;   ///< false: not expressible as a base instruction
	};

	/// @brief Expands one compressed instruction into its RV32I equivalent.
	/// @details Mirrors the interpreter's decoding of the RV32C quadrants. Only
	/// the encodings that map onto the base integer set are expanded; the F/D
	/// forms, the reserved encodings and C.ILLEGAL come back invalid, which makes
	/// them region terminators just like any other unsupported instruction.
	inline AjDecoded aj_expand_rvc(uint16_t whole) noexcept
	{
		const rv32c_instruction ci { whole };
		AjDecoded d;
		d.length = 2;
		auto& instr = d.instr;

		const auto addi = [&] (unsigned rd, unsigned rs1, int32_t imm) {
			instr.Itype.opcode = RV32I_OP_IMM;
			instr.Itype.funct3 = 0b000;
			instr.Itype.rd = rd; instr.Itype.rs1 = rs1;
			instr.Itype.imm = imm;
			d.valid = true;
		};
		const auto shift_or_logic_imm = [&] (unsigned funct3, unsigned rd, unsigned rs1, uint32_t imm) {
			instr.Itype.opcode = RV32I_OP_IMM;
			instr.Itype.funct3 = funct3;
			instr.Itype.rd = rd; instr.Itype.rs1 = rs1;
			instr.Itype.imm = imm;
			d.valid = true;
		};
		const auto op = [&] (unsigned funct3, unsigned funct7, unsigned rd, unsigned rs1, unsigned rs2) {
			instr.Rtype.opcode = RV32I_OP;
			instr.Rtype.funct3 = funct3; instr.Rtype.funct7 = funct7;
			instr.Rtype.rd = rd; instr.Rtype.rs1 = rs1; instr.Rtype.rs2 = rs2;
			d.valid = true;
		};
		const auto load = [&] (unsigned funct3, unsigned rd, unsigned rs1, uint32_t imm) {
			instr.Itype.opcode = RV32I_LOAD;
			instr.Itype.funct3 = funct3;
			instr.Itype.rd = rd; instr.Itype.rs1 = rs1;
			instr.Itype.imm = imm;
			d.valid = true;
		};
		const auto store = [&] (unsigned funct3, unsigned rs1, unsigned rs2, uint32_t imm) {
			instr.Stype.opcode = RV32I_STORE;
			instr.Stype.funct3 = funct3;
			instr.Stype.rs1 = rs1; instr.Stype.rs2 = rs2;
			instr.Stype.imm1 = imm & 0x1F;
			instr.Stype.imm2 = (imm >> 5) & 0x7F;
			d.valid = true;
		};
		const auto jal = [&] (unsigned rd, int32_t imm) {
			instr.Jtype.opcode = RV32I_JAL;
			instr.Jtype.rd = rd;
			instr.Jtype.imm3 = (imm >> 1) & 0x3FF;
			instr.Jtype.imm2 = (imm >> 11) & 0x1;
			instr.Jtype.imm1 = (imm >> 12) & 0xFF;
			instr.Jtype.imm4 = (imm < 0);
			// The 11-bit C.J/C.JAL offset always fits the 20-bit J-type field.
			d.valid = (instr.Jtype.jump_offset() == imm);
		};
		const auto branch = [&] (unsigned funct3, unsigned rs1, int32_t imm) {
			instr.Btype.opcode = RV32I_BRANCH;
			instr.Btype.funct3 = funct3;
			instr.Btype.rs1 = rs1; instr.Btype.rs2 = 0;
			instr.Btype.imm2 = (imm >> 1) & 0xF;
			instr.Btype.imm3 = (imm >> 5) & 0x3F;
			instr.Btype.imm1 = (imm >> 11) & 0x1;
			instr.Btype.imm4 = (imm < 0);
			d.valid = (instr.Btype.signed_imm() == imm);
		};

		switch (ci.opcode())
		{
		// --- Quadrant 0 ---
		case RISCV_CI_CODE(0b000, 0b00):   // C.ADDI4SPN
			if (ci.whole != 0 && ci.CIW.offset() != 0)
				addi(ci.CIW.srd + 8, 2, ci.CIW.offset());
			break;
		case RISCV_CI_CODE(0b010, 0b00):   // C.LW
			load(0b010, ci.CL.srd + 8, ci.CL.srs1 + 8, ci.CL.offset());
			break;
		case RISCV_CI_CODE(0b110, 0b00):   // C.SW
			store(0b010, ci.CS.srs1 + 8, ci.CS.srs2 + 8, ci.CS.offset4());
			break;

		// --- Quadrant 1 ---
		case RISCV_CI_CODE(0b000, 0b01):   // C.NOP / C.ADDI
			addi(ci.CI.rd, ci.CI.rd, ci.CI.signed_imm());
			break;
		case RISCV_CI_CODE(0b001, 0b01):   // C.JAL (RV32 only)
			jal(1, ci.CJ.signed_imm());
			break;
		case RISCV_CI_CODE(0b010, 0b01):   // C.LI
			addi(ci.CI.rd, 0, ci.CI.signed_imm());
			break;
		case RISCV_CI_CODE(0b011, 0b01):
			if (ci.CI.rd == 2) {           // C.ADDI16SP
				if (ci.CI16.signed_imm() != 0)
					addi(2, 2, ci.CI16.signed_imm());
			} else if (ci.CI.rd != 0) {    // C.LUI
				instr.Utype.opcode = RV32I_LUI;
				instr.Utype.rd = ci.CI.rd;
				instr.Utype.imm = ci.CI.signed_imm();
				d.valid = (ci.CI.signed_imm() != 0);
			}
			break;
		case RISCV_CI_CODE(0b100, 0b01):
			switch (ci.CA.funct6 & 0x3) {
			case 0:   // C.SRLI
				shift_or_logic_imm(0b101, ci.CA.srd + 8, ci.CA.srd + 8, ci.CAB.shift_imm());
				break;
			case 1:   // C.SRAI
				shift_or_logic_imm(0b101, ci.CA.srd + 8, ci.CA.srd + 8, ci.CAB.shift_imm() | 0x400);
				break;
			case 2:   // C.ANDI
				shift_or_logic_imm(0b111, ci.CA.srd + 8, ci.CA.srd + 8, ci.CAB.signed_imm());
				break;
			case 3:
				switch (ci.CA.funct2 | (ci.CA.funct6 & 0x4)) {
				case 0: op(0b000, 0x20, ci.CA.srd + 8, ci.CA.srd + 8, ci.CA.srs2 + 8); break; // C.SUB
				case 1: op(0b100, 0x00, ci.CA.srd + 8, ci.CA.srd + 8, ci.CA.srs2 + 8); break; // C.XOR
				case 2: op(0b110, 0x00, ci.CA.srd + 8, ci.CA.srd + 8, ci.CA.srs2 + 8); break; // C.OR
				case 3: op(0b111, 0x00, ci.CA.srd + 8, ci.CA.srd + 8, ci.CA.srs2 + 8); break; // C.AND
				default: break;   // C.SUBW / C.ADDW / reserved: RV64 only
				}
				break;
			}
			break;
		case RISCV_CI_CODE(0b101, 0b01):   // C.J
			jal(0, ci.CJ.signed_imm());
			break;
		case RISCV_CI_CODE(0b110, 0b01):   // C.BEQZ
			branch(0b000, ci.CB.srs1 + 8, ci.CB.signed_imm());
			break;
		case RISCV_CI_CODE(0b111, 0b01):   // C.BNEZ
			branch(0b001, ci.CB.srs1 + 8, ci.CB.signed_imm());
			break;

		// --- Quadrant 2 ---
		case RISCV_CI_CODE(0b000, 0b10):   // C.SLLI
			if (ci.CI.rd != 0)
				shift_or_logic_imm(0b001, ci.CI.rd, ci.CI.rd, ci.CI.shift_imm());
			break;
		case RISCV_CI_CODE(0b010, 0b10):   // C.LWSP
			if (ci.CI2.rd != 0)
				load(0b010, ci.CI2.rd, 2, ci.CI2.offset());
			break;
		case RISCV_CI_CODE(0b100, 0b10): {
			const bool topbit = ci.whole & (1 << 12);
			if (ci.CR.rd != 0 && ci.CR.rs2 == 0) {
				// C.JR rd / C.JALR rd
				instr.Itype.opcode = RV32I_JALR;
				instr.Itype.funct3 = 0b000;
				instr.Itype.rd  = topbit ? 1 : 0;
				instr.Itype.rs1 = ci.CR.rd;
				instr.Itype.imm = 0;
				d.valid = true;
			}
			else if (!topbit && ci.CR.rd != 0 && ci.CR.rs2 != 0) // C.MV
				addi(ci.CR.rd, ci.CR.rs2, 0);
			else if (topbit && ci.CR.rd != 0 && ci.CR.rs2 != 0)  // C.ADD
				op(0b000, 0x00, ci.CR.rd, ci.CR.rd, ci.CR.rs2);
			// C.EBREAK and the hint encodings stay invalid: they end the region.
			} break;
		case RISCV_CI_CODE(0b110, 0b10):   // C.SWSP
			store(0b010, 2, ci.CSS.rs2, ci.CSS.offset(4));
			break;

		default:
			// C.FLD/C.FLW/C.FSD/C.FSW and their SP-relative forms, plus every
			// reserved encoding: not part of the base integer set.
			break;
		}
		return d;
	}

	/// @brief Reads one guest instruction and normalizes it to 32-bit RV32I form.
	template <int W>
	inline AjDecoded aj_decode(const uint8_t* seg, address_type<W> pc, address_type<W> seg_end) noexcept
	{
		const auto raw = read_instruction(seg, pc, seg_end);
		if (raw.length() == 4) {
			if (pc + 4 > seg_end)
				return {};
			return { raw, 4, true };
		}
		if (pc + 2 > seg_end)
			return {};
#ifdef RISCV_EXT_C
		return aj_expand_rvc(raw.half[0]);
#else
		return {};
#endif
	}
}
