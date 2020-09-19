// -*-C++-*-

#ifdef RISCV_EXT_COMPRESSED
		if (instruction.is_long()) // RV32 IMAFD
		{
#endif
			// Quadrant 3
			switch (instruction.opcode())
			{
				// RV32IM
				case 0b0000011:
					switch (instruction.Stype.funct3) {
						case 0x0:
							DECODER(DECODED_INSTR(LOAD_I8));
						case 0x1:
							DECODER(DECODED_INSTR(LOAD_I16));
						case 0x2:
							DECODER(DECODED_INSTR(LOAD_I32));
						case 0x3:
							if constexpr (RVIS64BIT(*this)) {
								DECODER(DECODED_INSTR(LOAD_U64));
							}
							break;
						case 0x4:
							DECODER(DECODED_INSTR(LOAD_U8));
						case 0x5:
							DECODER(DECODED_INSTR(LOAD_U16));
						case 0x6:
							DECODER(DECODED_INSTR(LOAD_U32));
						default:
							DECODER(DECODED_INSTR(ILLEGAL));
					}
					break;
				case 0b0100011:
					switch (instruction.Stype.funct3) {
						case 0x0:
							DECODER(DECODED_INSTR(STORE_I8));
						case 0x1:
							DECODER(DECODED_INSTR(STORE_I16));
						case 0x2:
							DECODER(DECODED_INSTR(STORE_I32));
						case 0x3:
							if constexpr (RVIS64BIT(*this)) {
								DECODER(DECODED_INSTR(STORE_I64));
							}
							break;
						default:
							DECODER(DECODED_INSTR(ILLEGAL));
					}
					break;
				case 0b1100011:
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
				case 0b1100111:
					DECODER(DECODED_INSTR(JALR));
				case 0b1101111:
					DECODER(DECODED_INSTR(JAL));
				case 0b0010011: // OP_IMM
					if (instruction.Itype.rd != 0)
					{
						switch (instruction.Itype.funct3) {
						case 0x0: // ADDI
							DECODER(DECODED_INSTR(OP_IMM_ADDI));
						default:
							DECODER(DECODED_INSTR(OP_IMM));
						}
					}
					DECODER(DECODED_INSTR(NOP));
				case 0b0110011: // OP
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
				case 0b1110011:
					if (instruction.Itype.funct3 == 0)
					{
						if (instruction.Itype.imm == 0) {
							DECODER(DECODED_INSTR(SYSCALL));
						} else if (instruction.Itype.imm == 1) {
							DECODER(DECODED_INSTR(EBREAK));
						}
					}
					DECODER(DECODED_INSTR(SYSTEM));
				case 0b0110111:
					DECODER(DECODED_INSTR(LUI));
				case 0b0010111:
					DECODER(DECODED_INSTR(AUIPC));
				case 0b0011011: // OP_IMM32
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
				case 0b0111011: // OP32
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
				case 0b0001111:
					DECODER(DECODED_INSTR(FENCE));
#ifdef RISCV_EXT_FLOATS
				// RV32F & RV32D - Floating-point instructions
				case 0b0000111:
					DECODER(DECODED_FLOAT(FLW_FLD));
				case 0b0100111:
					DECODER(DECODED_FLOAT(FSW_FSD));
				case 0b1000011:
					DECODER(DECODED_FLOAT(FMADD));
				case 0b1000111:
					DECODER(DECODED_FLOAT(FMSUB));
				case 0b1001011:
					DECODER(DECODED_FLOAT(FNMSUB));
				case 0b1001111:
					DECODER(DECODED_FLOAT(FNMADD));
				case 0b1010011:
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
							DECODER(DECODED_FLOAT(FEQ_FLT_FLE));
						case 0b01000:
							DECODER(DECODED_FLOAT(FCVT_SD_DS));
						case 0b11000:
							DECODER(DECODED_FLOAT(FCVT_W_SD));
						case 0b11010:
							DECODER(DECODED_FLOAT(FCVT_SD_W));
						case 0b11100:
							DECODER(DECODED_FLOAT(FMV_X_W));
						case 0b11110:
							DECODER(DECODED_FLOAT(FMV_W_X));
					}
					break;
#endif
#ifdef RISCV_EXT_ATOMICS
				// RVxA - Atomic instructions
				case 0b0101111:
					switch (instruction.Atype.funct5)
					{
						case 0b00010:
							DECODER(DECODED_ATOMIC(LOAD_RESV));
						case 0b00011:
							DECODER(DECODED_ATOMIC(STORE_COND));
						case 0b00000:
							DECODER(DECODED_ATOMIC(AMOADD));
						case 0b00001:
							DECODER(DECODED_ATOMIC(AMOSWAP));
						case 0b01000:
							DECODER(DECODED_ATOMIC(AMOOR));
					}
#endif
			}
#ifdef RISCV_EXT_COMPRESSED
		}
		else if constexpr (compressed_enabled)
		{
			// RV32 C
			const auto ci = instruction.compressed();
			switch (ci.opcode())
			{
				// Quadrant 0
				case CI_CODE(0b000, 0b00):
					DECODER(DECODED_COMPR(C0_ADDI4SPN));
				case CI_CODE(0b001, 0b00):
				case CI_CODE(0b010, 0b00):
				case CI_CODE(0b011, 0b00):
					DECODER(DECODED_COMPR(C0_REG_LOAD));
				// RESERVED: 0b100, 0b00
				case CI_CODE(0b101, 0b00):
				case CI_CODE(0b110, 0b00):
				case CI_CODE(0b111, 0b00):
					DECODER(DECODED_COMPR(C0_REG_STORE));
				// Quadrant 1
				case CI_CODE(0b000, 0b01):
					DECODER(DECODED_COMPR(C1_NOP_ADDI));
				case CI_CODE(0b001, 0b01):
					if constexpr (RVIS64BIT(*this)) {
						DECODER(DECODED_COMPR(C1_ADDIW));
					} else {
						DECODER(DECODED_COMPR(C1_JAL));
					}
				case CI_CODE(0b010, 0b01):
					DECODER(DECODED_COMPR(C1_LI));
				case CI_CODE(0b011, 0b01):
					DECODER(DECODED_COMPR(C1_ADDI16SP_LUI));
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
					DECODER(DECODED_COMPR(C2_SP_LOAD));
				case CI_CODE(0b100, 0b10):
					DECODER(DECODED_COMPR(C2_VARIOUS));
				case CI_CODE(0b101, 0b10):
				case CI_CODE(0b110, 0b10):
				case CI_CODE(0b111, 0b10):
					DECODER(DECODED_COMPR(C2_SP_STORE));
			}
		}
#endif
		// illegal operation exception
		DECODER(DECODED_INSTR(UNIMPLEMENTED));
