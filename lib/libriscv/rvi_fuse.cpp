template <typename T>
T& view_as(rv32i_instruction& i) {
	static_assert(sizeof(T) == sizeof(i), "Must be same size as instruction!");
	return *(T*) &i;
}

template <int W>
inline bool fused_li_ecall(typename CPU<W>::instr_pair& i1, typename CPU<W>::instr_pair& i2, int sysno)
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
		cpu.registers().pc += fop.ilen;
		cpu.reg(RISCV::REG_ECALL) = fop.sysno;
		cpu.machine().unchecked_system_call(fop.sysno);
	};
	return true;
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
		const auto sysno = i1.second.Itype.signed_imm();
		if (i1.second.Itype.rd == RISCV::REG_ECALL && sysno < RISCV_SYSCALLS_MAX)
		{
			return fused_li_ecall<W>(i1, i2, sysno);
		}
	}
# ifdef RISCV_EXT_COMPRESSED
	else if (i1.first == DECODED_INSTR(C1_LI).handler &&
		i2.first == DECODED_INSTR(SYSCALL).handler)
	{
		const auto ci = i1.second.compressed();
		const auto sysno = ci.CI.signed_imm();
		if (ci.CI.rd == RISCV::REG_ECALL && sysno < RISCV_SYSCALLS_MAX)
		{
			return fused_li_ecall<W>(i1, i2, sysno);
		}
	}
# endif
#else
	(void) i1;
	(void) i2;
#endif
	return false;
}
