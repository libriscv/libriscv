
template <int W>
rv32i_instruction Emitter<W>::emit_rvc()
{
	#define CI_CODE(x, y) ((x << 13) | (y))
	const rv32c_instruction ci { instr };

	switch (ci.opcode())
	{
		case CI_CODE(0b000, 0b00): // C.ADDI4SPN
			if (ci.whole != 0) {
				instr.Itype.opcode = RV32I_OP_IMM;
				instr.Itype.funct3 = 0b000; // ADDI
				instr.Itype.rd = ci.CIW.srd + 8;
				instr.Itype.rs1 = 2; // sp
				instr.Itype.imm = ci.CIW.offset();
			}
			break;
		case CI_CODE(0b000, 0b01): { // C.ADDI
				instr.Itype.opcode = RV32I_OP_IMM;
				instr.Itype.funct3 = 0b000; // ADDI
				instr.Itype.rd = ci.CI.rd;
				instr.Itype.rs1 = ci.CI.rd;
				instr.Itype.imm = ci.CI.signed_imm();
			}
			break;
		case CI_CODE(0b010, 0b01): { // C.LI
			instr.Itype.opcode = RV32I_OP_IMM;
			instr.Itype.funct3 = 0b000; // ADDI
			instr.Itype.rd = ci.CI.rd;
			instr.Itype.rs1 = 0; // x0
			instr.Itype.imm = ci.CI.signed_imm();
			}
			break;
		case CI_CODE(0b011, 0b01): // C.ADDI16SP & C.LUI
			if (ci.CI.rd == 2) { // C.ADDI16SP
				instr.Itype.opcode = RV32I_OP_IMM;
				instr.Itype.funct3 = 0b000; // ADDI
				instr.Itype.rd = 2; // sp
				instr.Itype.rs1 = 2; // sp
				instr.Itype.imm = ci.CI16.signed_imm();
			}
			else if (ci.CI.rd != 0) { // C.LUI
				instr.Utype.opcode = RV32I_LUI;
				instr.Utype.rd = ci.CI.rd;
				instr.Utype.imm = ci.CI.signed_imm();
			}
			break; // C.ILLEGAL?
		case CI_CODE(0b001, 0b01):
			if constexpr (W == 4) {
				instr.Jtype.opcode = RV32I_JAL;
				instr.Jtype.rd = 1; // ra
				const auto imm = ci.CJ.signed_imm();
				instr.Jtype.imm3 = (imm >> 1) & 0x3FF;
				instr.Jtype.imm2 = (imm >> 11) & 0x1;
				instr.Jtype.imm1 = (imm >> 12) & 0xFF;
				instr.Jtype.imm4 = (imm < 0);

				if (instr.Jtype.jump_offset() != imm)
					throw MachineException(INVALID_PROGRAM, "Failed to sign-extend C.JAL immediate");
			} else { // C.ADDIW
				instr.Itype.opcode = RV64I_OP_IMM32;
				instr.Itype.funct3 = 0b000; // ADDIW
				instr.Itype.rd = ci.CI.rd;
				instr.Itype.rs1 = ci.CI.rd;
				instr.Itype.imm = ci.CI.signed_imm();
			}
			break;
		case CI_CODE(0b101, 0b01): { // C.JMP
			instr.Jtype.opcode = RV32I_JAL;
			instr.Jtype.rd = 0;
			const auto imm = ci.CJ.signed_imm();
			instr.Jtype.imm3 = (imm >> 1) & 0x3FF;
			instr.Jtype.imm2 = (imm >> 11) & 0x1;
			instr.Jtype.imm1 = (imm >> 12) & 0xFF;
			instr.Jtype.imm4 = (imm < 0);

			if (instr.Jtype.jump_offset() != imm)
				throw MachineException(INVALID_PROGRAM, "Failed to sign-extend C.JMP immediate");
			}
			break;
		case CI_CODE(0b110, 0b01): { // C.BEQZ
			instr.Btype.opcode = RV32I_BRANCH;
			instr.Btype.funct3 = 0; // BEQ
			instr.Btype.rs1 = ci.CB.srs1 + 8;
			instr.Btype.rs2 = 0;
			const auto imm = ci.CB.signed_imm();
			instr.Btype.imm2 = (imm >> 1) & 0xF;
			instr.Btype.imm3 = (imm >> 5) & 0x3F;
			instr.Btype.imm1 = (imm >> 11) & 0x1;
			instr.Btype.imm4 = (imm < 0);

			if (instr.Btype.signed_imm() != imm)
				throw MachineException(INVALID_PROGRAM, "Failed to sign-extend C.BEQZ immediate");
			}
			break;
		case CI_CODE(0b111, 0b01): { // C.BNEZ
			instr.Btype.opcode = RV32I_BRANCH;
			instr.Btype.funct3 = 1; // BNE
			instr.Btype.rs1 = ci.CB.srs1 + 8;
			instr.Btype.rs2 = 0;
			const auto imm = ci.CB.signed_imm();
			instr.Btype.imm2 = (imm >> 1) & 0xF;
			instr.Btype.imm3 = (imm >> 5) & 0x3F;
			instr.Btype.imm1 = (imm >> 11) & 0x1;
			instr.Btype.imm4 = (imm < 0);

			if (instr.Btype.signed_imm() != imm)
				throw MachineException(INVALID_PROGRAM, "Failed to sign-extend C.BNEZ immediate");
			break;
			}
		case CI_CODE(0b100, 0b10): { // C.VARIOUS
			const bool topbit = ci.whole & (1 << 12);
			if (ci.CR.rd != 0 && ci.CR.rs2 == 0) {
				if (topbit) {
					// C.JALR ra, rd+0 (aka. RET)
					instr.Itype.opcode = RV32I_JALR;
					instr.Itype.rd = 1; // ra
					instr.Itype.rs1 = ci.CR.rd;
					instr.Itype.imm = 0;
				} else {
					// C.JR rd (Jump to register rd)
					instr.Itype.opcode = RV32I_JALR;
					instr.Itype.rd = 0; // x0
					instr.Itype.rs1 = ci.CR.rd;
					instr.Itype.imm = 0;
				}
			}
			else if (!topbit && ci.CR.rd != 0 && ci.CR.rs2 != 0)
			{	// C.MV rd, rs2
				instr.Itype.opcode = RV32I_OP_IMM;
				instr.Itype.funct3 = 0b000; // ADDI
				instr.Itype.rd  = ci.CR.rd;
				instr.Itype.rs1 = ci.CR.rs2;
				instr.Itype.imm = 0;
			}
			else if (ci.CR.rd != 0)
			{	// C.ADD rd, rd + rs2
				instr.Rtype.opcode = RV32I_OP;
				instr.Rtype.funct3 = 0b000; // ADD
				instr.Rtype.funct7 = 0;
				instr.Rtype.rd  = ci.CR.rd;
				instr.Rtype.rs1 = ci.CR.rd;
				instr.Rtype.rs2 = ci.CR.rs2;
			}
			else if (topbit && ci.CR.rd == 0 && ci.CR.rs2 == 0)
			{	// C.EBREAK
				instr.Itype.opcode = RV32I_SYSTEM;
				instr.Itype.funct3 = 0b000; // SYSTEM
				instr.Itype.rd = 0;
				instr.Itype.rs1 = 0;
				instr.Itype.imm = 0x001; // EBREAK
			}
		} break;
	} // switch
	#undef CI_CODE
	return instr;
}
