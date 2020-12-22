// -*-C++-*-

#ifdef RISCV_EXT_COMPRESSED
		if (instruction.is_long()) // RV32 IMAFD
		{
#endif
			// Quadrant 3
			switch (instruction.opcode())
			{
				// RV32IM
				case RV32I_LOAD:
					switch (instruction.Itype.funct3) {
						case 0x0:
							if (instruction.Itype.rd != 0)
								DECODER(DECODED_INSTR(LOAD_I8));
							DECODER(DECODED_INSTR(LOAD_I8_DUMMY));
						case 0x1:
							if (instruction.Itype.rd != 0)
								DECODER(DECODED_INSTR(LOAD_I16));
							DECODER(DECODED_INSTR(LOAD_I16_DUMMY));
						case 0x2:
							if (instruction.Itype.rd != 0)
								DECODER(DECODED_INSTR(LOAD_I32));
							DECODER(DECODED_INSTR(LOAD_I32_DUMMY));
						case 0x3:
							if constexpr (RVIS64BIT(*this)) {
								if (instruction.Itype.rd != 0)
									DECODER(DECODED_INSTR(LOAD_U64));
								DECODER(DECODED_INSTR(LOAD_U64_DUMMY));
							}
							break;
						case 0x4:
							if (instruction.Itype.rd != 0)
								DECODER(DECODED_INSTR(LOAD_U8));
							DECODER(DECODED_INSTR(LOAD_I8_DUMMY));
						case 0x5:
							if (instruction.Itype.rd != 0)
								DECODER(DECODED_INSTR(LOAD_U16));
							DECODER(DECODED_INSTR(LOAD_I16_DUMMY));
						case 0x6:
							if (instruction.Itype.rd != 0)
								DECODER(DECODED_INSTR(LOAD_U32));
							DECODER(DECODED_INSTR(LOAD_I32_DUMMY));
						default:
							DECODER(DECODED_INSTR(ILLEGAL));
					}
					break;
				case RV32I_STORE:
					switch (instruction.Stype.funct3) {
						case 0x0:
							if (instruction.Stype.signed_imm() == 0)
								DECODER(DECODED_INSTR(STORE_I8));
							DECODER(DECODED_INSTR(STORE_I8_IMM));
						case 0x1:
							DECODER(DECODED_INSTR(STORE_I16_IMM));
						case 0x2:
							DECODER(DECODED_INSTR(STORE_I32_IMM));
						case 0x3:
							if constexpr (RVIS64BIT(*this)) {
								DECODER(DECODED_INSTR(STORE_I64_IMM));
							}
							break;
						default:
							DECODER(DECODED_INSTR(ILLEGAL));
					}
					break;
				case RV32I_BRANCH:
					switch (instruction.Btype.funct3) {
					case 0x0:
						DECODER(DECODED_INSTR(BRANCH_EQ));
					case 0x1:
						DECODER(DECODED_INSTR(BRANCH_NE));
					case 0x2:
					case 0x3:
						DECODER(DECODED_INSTR(ILLEGAL));
					case 0x4:
						DECODER(DECODED_INSTR(BRANCH_LT));
					case 0x5:
						DECODER(DECODED_INSTR(BRANCH_GE));
					case 0x6:
						DECODER(DECODED_INSTR(BRANCH_LTU));
					case 0x7:
						DECODER(DECODED_INSTR(BRANCH_GEU));
					}
				case RV32I_JALR:
					DECODER(DECODED_INSTR(JALR));
				case RV32I_JAL:
					DECODER(DECODED_INSTR(JAL));
				case RV32I_OP_IMM:
					if (instruction.Itype.rd != 0)
					{
						switch (instruction.Itype.funct3) {
						case 0x0: // ADDI
							if (instruction.Itype.rs1 == 0) {
								DECODER(DECODED_INSTR(OP_IMM_LI));
							}
							DECODER(DECODED_INSTR(OP_IMM_ADDI));
						case 0x1: // SLLI
							DECODER(DECODED_INSTR(OP_IMM_SLLI));
						case 0x6: // ORI
							DECODER(DECODED_INSTR(OP_IMM_ORI));
						case 0x7: // ANDI
							DECODER(DECODED_INSTR(OP_IMM_ANDI));
						default:
							DECODER(DECODED_INSTR(OP_IMM));
						}
					}
					DECODER(DECODED_INSTR(NOP));
				case RV32I_OP:
					if (instruction.Rtype.rd != 0)
					{
						switch (instruction.Rtype.jumptable_friendly_op()) {
						case 0x0: // ADD / SUB
							if (!instruction.Rtype.is_f7()) {
								DECODER(DECODED_INSTR(OP_ADD));
							} else {
								DECODER(DECODED_INSTR(OP_SUB));
							}
						default:
							DECODER(DECODED_INSTR(OP));
						}
					}
					DECODER(DECODED_INSTR(NOP));
				case RV32I_SYSTEM:
					if (instruction.Itype.funct3 == 0)
					{
						if (instruction.Itype.imm == 0) {
							DECODER(DECODED_INSTR(SYSCALL));
						} else if (instruction.Itype.imm == 1) {
							DECODER(DECODED_INSTR(EBREAK));
						}
					}
					DECODER(DECODED_INSTR(SYSTEM));
				case RV32I_LUI:
					if (instruction.Utype.rd != 0) {
						DECODER(DECODED_INSTR(LUI));
					} else {
						DECODER(DECODED_INSTR(NOP));
					}
				case RV32I_AUIPC:
					if (instruction.Utype.rd != 0) {
						DECODER(DECODED_INSTR(AUIPC));
					} else {
						DECODER(DECODED_INSTR(NOP));
					}
				case RV64I_OP_IMM32:
					if (instruction.Itype.rd != 0)
					{
						switch (instruction.Itype.funct3) {
						case 0x0: // ADDIW
							DECODER(DECODED_INSTR(OP_IMM32_ADDIW));
						default:
							DECODER(DECODED_INSTR(OP_IMM32));
						}
					} else {
						DECODER(DECODED_INSTR(NOP));
					}
				case RV64I_OP32:
					if (instruction.Rtype.rd != 0)
					{
						switch (instruction.Rtype.jumptable_friendly_op()) {
						case 0x0: // ADDW / SUBW
							DECODER(DECODED_INSTR(OP32_ADDW));
						default:
							DECODER(DECODED_INSTR(OP32));
						}
					} else {
						DECODER(DECODED_INSTR(NOP));
					}
				case RV32I_FENCE:
					DECODER(DECODED_INSTR(FENCE));
#ifdef RISCV_EXT_FLOATS
				// RV32F & RV32D - Floating-point instructions
				case RV32F_LOAD:
					DECODER(DECODED_FLOAT(FLW_FLD));
				case RV32F_STORE:
					DECODER(DECODED_FLOAT(FSW_FSD));
				case RV32F_FMADD:
					DECODER(DECODED_FLOAT(FMADD));
				case RV32F_FMSUB:
					DECODER(DECODED_FLOAT(FMSUB));
				case RV32F_FNMSUB:
					DECODER(DECODED_FLOAT(FNMSUB));
				case RV32F_FNMADD:
					DECODER(DECODED_FLOAT(FNMADD));
				case RV32F_FPFUNC:
					switch (instruction.fpfunc())
					{
						case 0b00000:
							DECODER(DECODED_FLOAT(FADD));
						case 0b00001:
							DECODER(DECODED_FLOAT(FSUB));
						case 0b00010:
							DECODER(DECODED_FLOAT(FMUL));
						case 0b00011:
							DECODER(DECODED_FLOAT(FDIV));
						case 0b00100:
							DECODER(DECODED_FLOAT(FSGNJ_NX));
						case 0b00101:
							DECODER(DECODED_FLOAT(FMIN_FMAX));
						case 0b01011:
							DECODER(DECODED_FLOAT(FSQRT));
						case 0b10100:
							if (rv32f_instruction { instruction }.R4type.rd != 0)
								DECODER(DECODED_FLOAT(FEQ_FLT_FLE));
							DECODER(DECODED_INSTR(NOP));
						case 0b01000:
							DECODER(DECODED_FLOAT(FCVT_SD_DS));
						case 0b11000:
							if (rv32f_instruction { instruction }.R4type.rd != 0)
								DECODER(DECODED_FLOAT(FCVT_W_SD));
							DECODER(DECODED_INSTR(NOP));
						case 0b11010:
							DECODER(DECODED_FLOAT(FCVT_SD_W));
						case 0b11100:
							if (rv32f_instruction { instruction }.R4type.rd != 0)
								DECODER(DECODED_FLOAT(FMV_X_W));
							DECODER(DECODED_INSTR(NOP));
						case 0b11110:
							DECODER(DECODED_FLOAT(FMV_W_X));
					}
					break;
#endif
#ifdef RISCV_EXT_ATOMICS
				// RVxA - Atomic instructions
				case RV32A_ATOMIC:
					switch (instruction.Atype.funct3)
					{
					case 0x2:
						switch (instruction.Atype.funct5)
						{
						case 0b00010:
							if (instruction.Atype.rs2 == 0)
								DECODER(DECODED_ATOMIC(LOAD_RESV));
							DECODER(DECODED_INSTR(ILLEGAL));
						case 0b00011:
							DECODER(DECODED_ATOMIC(STORE_COND));
						case 0b00000:
							DECODER(DECODED_ATOMIC(AMOADD_W));
						case 0b00001:
							DECODER(DECODED_ATOMIC(AMOSWAP_W));
						case 0b00100:
							DECODER(DECODED_ATOMIC(AMOXOR_W));
						case 0b01000:
							DECODER(DECODED_ATOMIC(AMOOR_W));
						case 0b01100:
							DECODER(DECODED_ATOMIC(AMOAND_W));
						}
						break;
					case 0x3:
					if constexpr (RVIS64BIT(*this)) {
						switch (instruction.Atype.funct5)
						{
						case 0b00010:
							if (instruction.Atype.rs2 == 0)
								DECODER(DECODED_ATOMIC(LOAD_RESV));
							DECODER(DECODED_INSTR(ILLEGAL));
						case 0b00011:
							DECODER(DECODED_ATOMIC(STORE_COND));
						case 0b00000:
							DECODER(DECODED_ATOMIC(AMOADD_D));
						case 0b00001:
							DECODER(DECODED_ATOMIC(AMOSWAP_D));
						case 0b00100:
							DECODER(DECODED_ATOMIC(AMOXOR_D));
						case 0b01000:
							DECODER(DECODED_ATOMIC(AMOOR_D));
						case 0b01100:
							DECODER(DECODED_ATOMIC(AMOAND_D));
						}
						break;
					}
					}
#endif
			}
#ifdef RISCV_EXT_COMPRESSED
		}
		else if constexpr (compressed_enabled)
		{
			// RISC-V Compressed Extension
			const auto ci = instruction.compressed();
			switch (ci.opcode())
			{
				// Quadrant 0
				case CI_CODE(0b000, 0b00):
					// if all bits are zero, it's an illegal instruction
					if (ci.whole != 0x0) {
						DECODER(DECODED_COMPR(C0_ADDI4SPN));
					}
					DECODER(DECODED_INSTR(ILLEGAL));
				case CI_CODE(0b001, 0b00):
				case CI_CODE(0b010, 0b00):
				case CI_CODE(0b011, 0b00):
					if (ci.CL.funct3 == 0x1) { // C.FLD
						DECODER(DECODED_COMPR(C0_REG_FLD));
					}
					else if (ci.CL.funct3 == 0x2) { // C.LW
						DECODER(DECODED_COMPR(C0_REG_LW));
					}
					else if (ci.CL.funct3 == 0x3) {
						if constexpr (RVIS64BIT(*this)) { // C.LD
							DECODER(DECODED_COMPR(C0_REG_LD));
						} else { // C.FLW
							DECODER(DECODED_COMPR(C0_REG_FLW));
						}
					}
					DECODER(DECODED_INSTR(ILLEGAL));
				// RESERVED: 0b100, 0b00
				case CI_CODE(0b101, 0b00):
				case CI_CODE(0b110, 0b00):
				case CI_CODE(0b111, 0b00):
					switch (ci.CS.funct3) {
					case 4:
						DECODER(DECODED_INSTR(UNIMPLEMENTED));
					case 5: // C.FSD
						DECODER(DECODED_COMPR(C0_REG_FSD));
					case 6: // C.SW
						DECODER(DECODED_COMPR(C0_REG_SW));
					case 7: // C.SD / C.FSW
						if constexpr (RVIS64BIT(*this)) {
							DECODER(DECODED_COMPR(C0_REG_SD));
						} else {
							DECODER(DECODED_COMPR(C0_REG_FSW));
						}
					}
					DECODER(DECODED_INSTR(ILLEGAL));
				// Quadrant 1
				case CI_CODE(0b000, 0b01): // C.ADDI
					if (ci.CI.rd != 0) {
						DECODER(DECODED_COMPR(C1_ADDI));
					}
					DECODER(DECODED_INSTR(NOP));
				case CI_CODE(0b001, 0b01): // C.ADDIW / C.JAL
					if constexpr (RVIS64BIT(*this)) {
						if (ci.CI.rd != 0) {
							DECODER(DECODED_COMPR(C1_ADDIW));
						}
						DECODER(DECODED_INSTR(NOP));
					} else {
						DECODER(DECODED_COMPR(C1_JAL));
					}
				case CI_CODE(0b010, 0b01):
					if (ci.CI.rd != 0) {
						DECODER(DECODED_COMPR(C1_LI));
					}
					DECODER(DECODED_INSTR(NOP));
				case CI_CODE(0b011, 0b01):
					if (ci.CI.rd == 2) {
						DECODER(DECODED_COMPR(C1_ADDI16SP));
					}
					else if (ci.CI.rd != 0) {
						DECODER(DECODED_COMPR(C1_LUI));
					}
					DECODER(DECODED_INSTR(ILLEGAL));
				case CI_CODE(0b100, 0b01):
					DECODER(DECODED_COMPR(C1_ALU_OPS));
				case CI_CODE(0b101, 0b01):
					DECODER(DECODED_COMPR(C1_JUMP));
				case CI_CODE(0b110, 0b01):
					DECODER(DECODED_COMPR(C1_BEQZ));
				case CI_CODE(0b111, 0b01):
					DECODER(DECODED_COMPR(C1_BNEZ));
				// Quadrant 2
				case CI_CODE(0b000, 0b10):
				case CI_CODE(0b001, 0b10):
				case CI_CODE(0b010, 0b10):
				case CI_CODE(0b011, 0b10):
					if (ci.CI.funct3 == 0x0 && ci.CI.rd != 0) {
						// C.SLLI
						DECODER(DECODED_COMPR(C2_SLLI));
					}
					else if (ci.CI2.funct3 == 0x1) {
						// C.FLDSP
						DECODER(DECODED_COMPR(C2_FLDSP));
					}
					else if (ci.CI2.funct3 == 0x2 && ci.CI2.rd != 0) {
						// C.LWSP
						DECODER(DECODED_COMPR(C2_LWSP));
					}
					else if (ci.CI2.funct3 == 0x3) {
						if constexpr (RVIS64BIT(*this)) {
							// C.LDSP
							DECODER(DECODED_COMPR(C2_LDSP));
						} else {
							// C.FLWSP
							DECODER(DECODED_COMPR(C2_FLWSP));
						}
					}
					else if (ci.CI.rd == 0) {
						// C.HINT
						DECODER(DECODED_INSTR(NOP));
					}
					DECODER(DECODED_COMPR(UNIMPLEMENTED));
				case CI_CODE(0b100, 0b10): {
					const bool topbit = ci.whole & (1 << 12);
					if (!topbit && ci.CR.rd != 0 && ci.CR.rs2 == 0)
					{	// JR rd
						DECODER(DECODED_COMPR(C2_JR));
					}
					else if (topbit && ci.CR.rd != 0 && ci.CR.rs2 == 0)
					{	// JALR ra, rd+0
						DECODER(DECODED_COMPR(C2_JALR));
					}
					else if (!topbit && ci.CR.rd != 0 && ci.CR.rs2 != 0)
					{	// MV rd, rs2
						DECODER(DECODED_COMPR(C2_MV));
					}
					else if (ci.CR.rd != 0)
					{	// ADD rd, rd + rs2
						DECODER(DECODED_COMPR(C2_ADD));
					}
					else if (topbit && ci.CR.rd == 0 && ci.CR.rs2 == 0)
					{	// EBREAK
						DECODER(DECODED_COMPR(C2_EBREAK));
					}
					DECODER(DECODED_COMPR(UNIMPLEMENTED));
				}
				case CI_CODE(0b101, 0b10):
				case CI_CODE(0b110, 0b10):
				case CI_CODE(0b111, 0b10):
					if (ci.CSS.funct3 == 5) {
						// FSDSP
						DECODER(DECODED_COMPR(C2_FSDSP));
					}
					else if (ci.CSS.funct3 == 6) {
						// SWSP
						DECODER(DECODED_COMPR(C2_SWSP));
					}
					else if (ci.CSS.funct3 == 7) {
						if constexpr (RVIS64BIT(*this)) {
							// SDSP
							DECODER(DECODED_COMPR(C2_SDSP));
						} else {
							// FSWSP
							DECODER(DECODED_COMPR(C2_FSWSP));
						}
					}
					DECODER(DECODED_COMPR(UNIMPLEMENTED));
			}
		}
#endif
		// all zeroes: illegal instruction
		if (instruction.whole == 0x0) {
			DECODER(DECODED_INSTR(ILLEGAL));
		}
		DECODER(DECODED_INSTR(UNIMPLEMENTED));
