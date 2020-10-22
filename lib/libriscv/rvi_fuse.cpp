template <typename T>
T& view_as(rv32i_instruction& i) {
	static_assert(sizeof(T) == sizeof(i), "Must be same size as instruction!");
	return *(T*) &i;
}

template <int W>
bool CPU<W>::try_fuse(instr_pair i1, instr_pair i2) const
{
#ifdef RISCV_INSTR_CACHE_PREGEN
	// LI + ECALL fused
	if (i1.first == DECODED_INSTR(OP_IMM_LI).handler) {
		if (i2.first == DECODED_INSTR(SYSCALL).handler) {
			i1.first = [] (auto& cpu, rv32i_instruction instr) {
				cpu.registers().pc += 4;
				cpu.reg(instr.Itype.rd) = instr.Itype.signed_imm();
				cpu.machine().system_call(cpu.reg(RISCV::REG_ECALL));
			};
			return true;
		}
	}
	// LI + LI fused
	if (i1.first == DECODED_INSTR(OP_IMM_LI).handler) {
		if (i2.first == DECODED_INSTR(OP_IMM_LI).handler) {
			// not enough room for upper bits
			if (i1.second.Itype.rd >= 32 || i2.second.Itype.rd >= 32) {
				return false;
			}
			// Unfortunately, this clobbers the bits needed to
			// measure the length of the instruction, so it will randomly
			// see a 16-bit instruction and move on to an invalid opcode.
			return false;

			struct Lili {
				uint16_t    imm1   : 12;
				uint16_t    rd1    : 4;
				uint16_t    imm2   : 12;
				uint16_t    rd2    : 4;

				static bool sign(uint32_t imm) {
					return imm & 0x800;
				}
				static int64_t signed_imm(uint32_t imm) {
					const uint64_t ext = 0xFFFFFFFFFFFFF000;
					return imm | (sign(imm) ? ext : 0);
				}
			};
			auto copy = i1.second;
			auto& lili = view_as<Lili> (i1.second);
			lili.imm1 = copy.Itype.imm;
			lili.rd1 = copy.Itype.rd;
			lili.imm2 = i2.second.Itype.imm;
			lili.rd2 = i2.second.Itype.rd;

			i1.first = [] (auto& cpu, rv32i_instruction instr) {
				auto& lili = view_as<Lili> (instr);
				cpu.reg(lili.rd1) = Lili::signed_imm(lili.imm1);
				cpu.reg(lili.rd2) = Lili::signed_imm(lili.imm2);
				cpu.registers().pc += 4;
			};
			return true;
		}
	}
#endif
	return false;
}
