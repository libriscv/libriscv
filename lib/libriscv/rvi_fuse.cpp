template <typename T>
T& view_as(rv32i_instruction& i) {
	static_assert(sizeof(T) == sizeof(i), "Must be same size as instruction!");
	return *(T*) &i;
}

template <int W>
inline void fused_li_ecall(
	typename CPU<W>::instr_pair& i1, typename CPU<W>::instr_pair& i2, int sysno)
{
	union FusedSyscall {
		struct {
			uint8_t lower;
			uint8_t ilen;
			uint16_t sysno;
		};
		uint32_t whole;
	};
	i1.second.whole = FusedSyscall {
		.lower = (uint8_t)  i2.second.half[0],  // Trick emulator to step
		.ilen  = (uint8_t)  i1.second.length(), // over second instruction.
		.sysno = (uint16_t) sysno,
	}.whole;
	i1.first = [] (auto& cpu, rv32i_instruction instr) {
		auto& fop = view_as<FusedSyscall> (instr);
		if constexpr (compressed_enabled)
			cpu.registers().pc += fop.ilen;
		else
			cpu.registers().pc += 4;
		cpu.reg(RISCV::REG_ECALL) = fop.sysno;
		cpu.machine().unchecked_system_call(fop.sysno);
	};
}

template <int W>
bool CPU<W>::try_fuse(instr_pair i1, instr_pair i2) const
{
#ifdef RISCV_INSTR_CACHE_PREGEN
	// LI + ECALL fused
	if (i1.first == DECODED_INSTR(OP_IMM_LI).handler &&
		i2.first == DECODED_INSTR(SYSCALL).handler)
	{
		// fastest possible system calls
		const uint16_t sysno = i1.second.Itype.signed_imm();
		if (i1.second.Itype.rd == RISCV::REG_ECALL && sysno < RISCV_SYSCALLS_MAX)
		{
			fused_li_ecall<W>(i1, i2, sysno);
			return true;
		}
	}
	// ADDI x, x + ADDI y, y fused
	if (i1.first == DECODED_INSTR(OP_IMM_ADDI).handler &&
		i2.first == DECODED_INSTR(OP_IMM_ADDI).handler)
	{
		if (i1.second.Itype.rd == i1.second.Itype.rs1 && i1.second.Itype.rd < 16)
		if (i2.second.Itype.rd == i2.second.Itype.rs1 && i2.second.Itype.rd < 16)
		if constexpr (!compressed_enabled)
		{
			union FusedAddi {
				struct {
					uint32_t addi1 : 12;
					uint32_t reg1  : 4;
					uint32_t addi2 : 12;
					uint32_t reg2  : 4;
				};
				static bool sign(uint32_t imm) {
					return imm & 0x800;
				}
				static int64_t signed_imm(uint32_t imm) {
					const uint64_t ext = 0xFFFFFFFFFFFFF000;
					return imm | (sign(imm) ? ext : 0);
				}
				uint32_t whole;
			};
			FusedAddi fop;
			fop.addi1 = i1.second.Itype.imm;
			fop.reg1  = i1.second.Itype.rd;
			fop.addi2 = i2.second.Itype.imm;
			fop.reg2  = i2.second.Itype.rd;
			i1.second.whole = fop.whole;
			i1.first = [] (auto& cpu, rv32i_instruction instr) {
				auto& fop = view_as<FusedAddi> (instr);
				cpu.reg(fop.reg1) += FusedAddi::signed_imm(fop.addi1);
				cpu.reg(fop.reg2) += FusedAddi::signed_imm(fop.addi2);
				cpu.registers().pc += 4;
			};
			return true;
		}
	}
# ifdef RISCV_EXT_COMPRESSED
	// C.LI + ECALL fused
	else if (i1.first == DECODED_INSTR(C1_LI).handler &&
		i2.first == DECODED_INSTR(SYSCALL).handler)
	{
		const auto ci = i1.second.compressed();
		const uint16_t sysno = ci.CI.signed_imm();
		if (ci.CI.rd == RISCV::REG_ECALL && sysno < RISCV_SYSCALLS_MAX)
		{
			fused_li_ecall<W>(i1, i2, sysno);
			return true;
		}
	}
# endif
#else
	(void) i1;
	(void) i2;
#endif
	return false;
}
