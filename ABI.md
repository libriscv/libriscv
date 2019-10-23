###

x0     ZERO
x1 RA  Return address
x2 SP  Stack pointer
x3 GP  Global pointer
x4 TP  Thread pointer
x5 LR  Link register
x6-7   Temporaries
x8 S0  Saved frame pointer
x9 S1  Saved register
x10-11 Function args & return values
x12-17 Function arguments
x18-27 Saved registers
x28-31 Temporaries

f0-7   FP temporaries
f8-9   FP saved registers
f10-11 FP args & return values
f12-17 FP arguments
f18-27 FP saved registers
f28-31 FP temporaries

PC    Program counter



All instructions 4-byte aligned! (Except for 16-bit extension)
The misalignment exception happens on the branch jump that would cause the misalignment.
