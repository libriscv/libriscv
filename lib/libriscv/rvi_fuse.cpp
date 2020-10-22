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
	if (i1.first == DECODED_INSTR(OP_IMM_LI).handler &&
		i2.first == DECODED_INSTR(SYSCALL).handler)
	{
		// fastest possible system calls
		const auto sysno = i1.second.Itype.signed_imm();
		if (i1.second.Itype.rd == RISCV::REG_ECALL && sysno < RISCV_SYSCALLS_MAX)
		{
			i1.second.whole = sysno;
			i1.first = [] (auto& cpu, rv32i_instruction instr) {
				cpu.registers().pc += 4;
				cpu.reg(RISCV::REG_ECALL) = instr.whole;
				cpu.machine().unchecked_system_call(instr.whole);
			};
			return true;
		}
	}
#else
	(void) i1;
	(void) i2;
#endif
	return false;
}
