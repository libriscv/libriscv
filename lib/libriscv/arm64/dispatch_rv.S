// RV32 and RV64 dispatch for libriscv, AArch64 (AAPCS64, ELF).
//
// The AArch64 counterpart of amd64/dispatch_rv.nasm: the same decoder entries,
// the same dispatch table filled in by C++ (amd64/asm_dispatch.cpp), the same
// context, exit codes and calls into C++ (asm_dispatch_glue.hpp). Assemble once
// per register width and mode, through the C preprocessor:
//   cc -c -x assembler-with-cpp -DXLEN=32|64 -DCOUNTING=0|1 -DCOMPRESSED=0|1
//      -DARENA_BITS=0|N
// COUNTING=0 is CPU::simulate_inaccurate (symbols prefixed rv32asm_/rv64asm_),
// COUNTING=1 counts instructions like cpu_dispatch.cpp (rv32asmc_/rv64asmc_).
// ARENA_BITS=N must match RISCV_ENCOMPASSING_ARENA_BITS: the arena then covers
// the whole N-bit address space, and guest memory is arena[address mod 2^N],
// without bounds checks, like Memory::read and Memory::write. 0 = the regular
// flat arena, with bounds checks.
// See dispatch_rv.nasm for the decoder entry layouts and the semantics, which
// this file follows exactly.
//
// This is the straightforward version: one copy of each handler, and none of
// the Zen 4 tuning of the AMD64 dispatch (handler replication, backward branch
// stubs, self-branches in memory handlers). Fused pairs do have their own
// handlers, see FUSED. Only base ARMv8.0-A is used.

#ifndef XLEN
#define XLEN 64
#endif
#ifndef COMPRESSED
#define COMPRESSED 0
#endif
#ifndef COUNTING
#define COUNTING 0
#endif
#ifndef ARENA_BITS
#define ARENA_BITS 0
#endif
#if ARENA_BITS != 0 && (ARENA_BITS < 12 || ARENA_BITS > 48)
#error "ARENA_BITS must be 0 or 12..48"
#endif

#define CAT_(a, b) a##b
#define CAT(a, b) CAT_(a, b)
#if COUNTING
#define PFX CAT(CAT(rv, XLEN), asmc_)
#else
#define PFX CAT(CAT(rv, XLEN), asm_)
#endif
#define SYM(name) CAT(PFX, name)

// XP is the prefix of the host registers that hold a guest register value
// (or a guest address) of XLEN bits, XSH the log2 of its size in bytes.
#if XLEN == 64
#define XP x
#define XB 8
#define XSH 3
#define TZR xzr
#define FPOFF 272   // offsetof(Registers<8>, m_regfl)
#elif XLEN == 32
#define XP w
#define XB 4
#define XSH 2
#define TZR wzr
#define FPOFF 136   // offsetof(Registers<4>, m_regfl)
#else
#error "XLEN must be 32 or 64"
#endif
#define T0 CAT(XP, 0)
#define T1 CAT(XP, 1)
#define T2 CAT(XP, 2)
#define T3 CAT(XP, 3)
#define T4 CAT(XP, 4)

#if COMPRESSED
#define SHIFT 1
#else
#define SHIFT 2
#endif
// One decoder entry (8 bytes) per 2^SHIFT bytes of code
#define I4      (8 * (4 >> SHIFT))  // decoder bytes for a 4-byte instruction
#define I2      8                   // decoder bytes for a 2-byte instruction
#define PC2DEC  (3 - SHIFT)         // decoder offset = pc << PC2DEC

// Pinned state, all in callee-saved registers, so that it survives calls
// into C++ without saving anything:
//   x19 decoder entry pointer (current instruction)
//   x20 register file: x0..x31 (XB bytes each), then pc, fcsr, f0..f31
//   x21 decoder cache base (indexed by pc >> SHIFT)
//   x22 flat arena base
//   x23 dispatch table (256 x 8-byte absolute addresses)
//   x24 arena read boundary   (addr - 0x1000 < x24 is readable)
//   x25 initial rodata end
//   x26 arena write boundary  (addr - x25 < x26 is writable)
//       With ARENA_BITS there are no boundaries, and x24..x26 are unused.
//   x27 execute segment begin           (COUNTING: max counter)
//   x28 execute segment length          (COUNTING: counter)
// Every handler is entered with x9 = the whole 8-byte decoder entry:
//   bits 0..7 = bytecode, bits 32..63 = instruction operands (bytes 4..7).
// Scratch: x0..x15, v0..v3. x18 (platform register) is not used.
#define RENT   x19
#define RREGS  x20
#define RDEC   x21
#define RARENA x22
#define RTAB   x23
#define RRB    CAT(XP, 24)
#define RROD   CAT(XP, 25)
#define RWB    CAT(XP, 26)
#if COUNTING
#define RMAX   x27
#define RCOUNT x28
#else
#define RBEGIN CAT(XP, 27)
#define RLEN   CAT(XP, 28)
#endif

// Stack frame: x29/x30, x19..x28, then the context
#define FRAME    112
#define CTX_SLOT 96

// Must match AsmDispatchContext (asm_dispatch_glue.hpp)
#define CTX_REGS     0
#define CTX_DECODER  8
#define CTX_BEGIN    16
#define CTX_LENGTH   24
#define CTX_ARENA    32
#define CTX_RBOUND   40
#define CTX_RODATA   48
#define CTX_WBOUND   56
#define CTX_TABLE    64
#define CTX_PC       72
#define CTX_CPU      80
#define CTX_COUNTER  88
#define CTX_MAX      96
#define CTX_ENTER_COUNT 104

#define EXIT_STOP      0
#define EXIT_REENTER   1
#define EXIT_JUMP      2
#define EXIT_EXCEPTION 3
#define CALL_NEXT      4
#define CALL_GOTO      5
#define EXIT_OVERFLOW  6

#define KIND_FUNCTION   0
#define KIND_C_FUNCTION 1
#define KIND_SYSCALL    2
#define KIND_DYNCALL    3
#define KIND_SYSTEM     4
#define KIND_FUNCBLOCK  5
#define KIND_STEP       6
#define KIND_INVALID    7
#define KIND_TRANSLATOR 8
#define KIND_ASMJIT     9
#define KIND_LIVEPATCH  10
#define KIND_C_STEP     11

#define RWREAD_BEGIN 0x1000

// Guest memory at the address in x0 (XLEN bits, or ARENA_BITS when <= 32)
#if XLEN == 64 && (ARENA_BITS == 0 || ARENA_BITS > 32)
#define MEM [RARENA, x0]
#else
#define MEM [RARENA, w0, uxtw]
#endif

// -== Operand fields of the entry in x9 ==-
.macro F_B4 r            // byte 4
	ubfx \r, x9, #32, #8
.endm
.macro F_B5 r            // byte 5
	ubfx \r, x9, #40, #8
.endm
.macro F_B6 r            // byte 6
	ubfx \r, x9, #48, #8
.endm
.macro F_B7 r            // byte 7
	lsr \r, x9, #56
.endm
.macro F_S4 r            // signed 16 bits at 4
	sbfx \r, x9, #32, #16
.endm
.macro F_U4 r            // unsigned 16 bits at 4
	ubfx \r, x9, #32, #16
.endm
.macro F_S6 r            // signed 16 bits at 6
	asr \r, x9, #48
.endm

// Guest integer register: \r (XLEN-wide) = / from REG(\idx), \idx an x-register
.macro LDREG r, idx
	ldr \r, [RREGS, \idx, lsl #XSH]
.endm
.macro STREG r, idx
	str \r, [RREGS, \idx, lsl #XSH]
.endm
// \dst (x-register) = address of FREG(\idx) - FPOFF
.macro FREG_ADDR dst, idx
	add \dst, RREGS, \idx, lsl #3
.endm

// -== Dispatch ==-

.macro DISPATCH
	ldr x9, [RENT]
	and x10, x9, #0xff
	ldr x10, [RTAB, x10, lsl #3]
	br x10
.endm
// Advance the decoder pointer by \n bytes and dispatch
.macro NEXT n
	ldr x9, [RENT, #\n]!
	and x10, x9, #0xff
	ldr x10, [RTAB, x10, lsl #3]
	br x10
.endm
.macro NEXT_INSTR
	NEXT I4
.endm
.macro NEXT_C_INSTR
	NEXT I2
.endm

// Dispatch the decoder entry at x19, which begins a block. COUNTING: count it.
.macro DISPATCH_BLOCK
	ldr x9, [RENT]
#if COUNTING && COMPRESSED
	ubfx x10, x9, #24, #8   // icount: the instructions from here to the block end
	add RCOUNT, RCOUNT, x10
#elif COUNTING
	ubfx x10, x9, #16, #16  // idxend: the instructions after this one in its block
	add RCOUNT, RCOUNT, x10
	add RCOUNT, RCOUNT, #1
#endif
	and x10, x9, #0xff
	ldr x10, [RTAB, x10, lsl #3]
	br x10
.endm
// Advance by \n bytes to the next block (no overflow check)
.macro NEXT_BLOCK n
#if COUNTING
	add RENT, RENT, #\n
	DISPATCH_BLOCK
#else
	NEXT \n
#endif
.endm
.macro NEXT_BLOCK_INSTR
	NEXT_BLOCK I4
.endm
.macro NEXT_BLOCK_C_INSTR
	NEXT_BLOCK I2
.endm

// COUNTING: leave the dispatch when the counter has reached the max, at the
// pc of the decoder entry in x19 (_RBX) or at the guest address in x0 (_EAX)
.macro CHECK_OVERFLOW_RBX
#if COUNTING
	cmp RCOUNT, RMAX
	b.hs exit_overflow_rbx
#endif
.endm
.macro CHECK_OVERFLOW_EAX
#if COUNTING
	cmp RCOUNT, RMAX
	b.hs exit_overflow
#endif
.endm

// \dst (x-register) = the pc of the current instruction
.macro RECONSTRUCT_PC dst
	sub \dst, RENT, RDEC
	lsr \dst, \dst, #PC2DEC
.endm

// Branch by the signed 16-bit byte offset in the instruction.
// \check = 1: the branch may go backwards, so check the counter first
.macro TAKE_BRANCH check
	F_S4 x0
	add RENT, RENT, x0, lsl #PC2DEC
.if \check
	CHECK_OVERFLOW_RBX
.endif
	DISPATCH_BLOCK
.endm

// Jump to the guest address in x0: within the segment it is just a new
// decoder pointer, otherwise C++ has to find the segment.
// Spectre v1: the guest controls every bit of x0, so on a mispredicted b.hs
// RENT would point at arbitrary host memory, and the predicted handler would
// take its operands from there. The csel clamps the address to the segment
// begin; it never selects it architecturally (C=0 here), and CSDB stops the
// csel from being predicted.
.macro JUMP_TO_EAX
#if COUNTING
	CHECK_OVERFLOW_EAX
	ldr x1, [sp, #CTX_SLOT]
	ldr T2, [x1, #CTX_BEGIN]
	ldr T3, [x1, #CTX_LENGTH]
	sub T4, T0, T2
	cmp T4, T3
	b.hs exit_jump
	csel T0, T2, T0, hs
#else
	sub T2, T0, RBEGIN
	cmp T2, RLEN
	b.hs exit_jump
	csel T0, RBEGIN, T0, hs
#endif
	hint #20                // csdb
#if XLEN == 64
	add RENT, RDEC, x0, lsl #PC2DEC
#else
	add RENT, RDEC, w0, uxtw #PC2DEC
#endif
	DISPATCH_BLOCK
.endm

// Load the arena boundaries from the context in \ctx
.macro LOAD_ARENA_BOUNDS ctx
#if ARENA_BITS == 0
	ldr RRB,  [\ctx, #CTX_RBOUND]
	ldr RROD, [\ctx, #CTX_RODATA]
	ldr RWB,  [\ctx, #CTX_WBOUND]
#endif
.endm

// Execute the current decoder entry in C++, see <prefix>call.
// \kind = KIND_*, \next = the macro that continues with the next instruction
.macro CALL_CPP kind, next
	ldr x0, [sp, #CTX_SLOT]
#if COUNTING
	str RCOUNT, [x0, #CTX_COUNTER]
	str RMAX, [x0, #CTX_MAX]
#endif
	mov x1, RENT
	mov w2, #\kind
	bl SYM(call)
	// The arena boundaries can change in any system call
	ldr x1, [sp, #CTX_SLOT]
	LOAD_ARENA_BOUNDS x1
#if COUNTING
	// So can the counters (penalties, stopping)
	ldr RCOUNT, [x1, #CTX_COUNTER]
	ldr RMAX, [x1, #CTX_MAX]
#endif
	cmp w0, #CALL_NEXT
	b.ne 1f
	\next
1:	b.hi call_goto
	b exit_dispatch         // w0 = exit code, ctx->pc written by C++
.endm

.macro ADDRESS at
.if \at == 6
	F_B6 x1
.else
	F_B7 x1
.endif
	F_S4 x2
	LDREG T1, x1
#if ARENA_BITS != 0 && ARENA_BITS <= 32
	add w0, w1, w2          // the low 32 bits are all the mask keeps
#if ARENA_BITS < 32
	and w0, w0, #((1 << ARENA_BITS) - 1)
#endif
#else
	add T0, T1, T2
#if ARENA_BITS != 0
	and x0, x0, #((1 << ARENA_BITS) - 1)
#endif
#endif
.endm

#if ARENA_BITS != 0
// Encompassing arena: x0 = (REG(rs @ byte \at) + imm16) mod 2^ARENA_BITS,
// which is always inside the arena, so there are no checks and no fallback,
// and reads and writes are the same (C++ has no write protection here either).
// MEM zero-extends w0 when ARENA_BITS <= 32, and a 32-bit add truncates the
// address for free, which is uint32_t(address) in Memory::read. The mask is
// a logical immediate, so unlike AMD64 no register holds it for N > 32.
// Masking is a data dependency, so even a mispredicted branch cannot take a
// load or store outside the arena (Spectre v1 and v1.1).
.macro READ_ADDRESS at, fallback
	ADDRESS \at
.endm
.macro WRITE_ADDRESS at, fallback
	ADDRESS \at
.endm
#else
// x0 = guest address REG(rs @ byte \at) + imm16, or go to \fallback when
// it is not readable (READ) or writable (WRITE) in the flat arena
.macro READ_ADDRESS at, fallback
	ADDRESS \at
	sub T3, T0, #RWREAD_BEGIN
	cmp T3, RRB
	b.hs \fallback
.endm
.macro WRITE_ADDRESS at, fallback
	ADDRESS \at
	sub T3, T0, RROD
	cmp T3, RWB
	b.hs \fallback
.endm
#endif // ARENA_BITS

// FasterItype: imm16 @4, rs2 (base) @6, rs1 (dst) @7
// \op \reg, MEM loads the value, \reg being x3 or w3, then T3 is stored
.macro LOAD op, reg, next=NEXT_INSTR, fallback=do_fallback
	READ_ADDRESS 6, \fallback
	\op \reg, MEM
	F_B7 x4
	STREG T3, x4
	\next
.endm
// FasterItype: imm16 @4, rs2 (value) @6, rs1 (base) @7
// \op \reg, MEM stores the value, \reg being x4, w4 (T4 = the value)
.macro STORE op, reg, next=NEXT_INSTR, fallback=do_fallback
	WRITE_ADDRESS 7, \fallback
	F_B6 x4
	LDREG T4, x4
	\op \reg, MEM
	\next
.endm
// Floating point, \reg = s0 or d0. Without NaN-boxing: C++ maps FLW only
// without it, and a single-precision load only writes the low half.
.macro FLOAD reg, next=NEXT_INSTR, fallback=do_fallback
	READ_ADDRESS 6, \fallback
	ldr \reg, MEM
	F_B7 x4
	FREG_ADDR x4, x4
	str \reg, [x4, #FPOFF]
	\next
.endm
.macro FSTORE reg, next=NEXT_INSTR, fallback=do_fallback
	WRITE_ADDRESS 7, \fallback
	F_B6 x4
	FREG_ADDR x4, x4
	ldr \reg, [x4, #FPOFF]
	str \reg, MEM
	\next
.endm

// ITYPE: T1 = REG(src @6), x2 = imm field (\signed: sign-extended), x0 = dst @7
.macro ITYPE signed=1
	F_B6 x1
.if \signed
	F_S4 x2
.else
	F_U4 x2
.endif
	F_B7 x0
	LDREG T1, x1
.endm
// OP: T2 = REG(rs1 @7), T1 = REG(rs2 @6), x0 = rd16 @4
.macro OPTYPE
	F_B7 x2
	F_B6 x1
	F_U4 x0
	LDREG T2, x2
	LDREG T1, x1
.endm
// T3 = T2 \op T1, stored in REG(rd)
.macro OP_ARITH op
	OPTYPE
	\op T3, T2, T1
	STREG T3, x0
	NEXT_INSTR
.endm
// T1 = T1 \op imm, stored in REG(dst)
.macro IMM_ARITH op, signed=1
	ITYPE \signed
	\op T1, T1, T2
	STREG T1, x0
	NEXT_INSTR
.endm
.macro BRANCH cond, check
	F_B7 x0
	F_B6 x1
	LDREG T0, x0
	LDREG T1, x1
	cmp T0, T1
	b.\cond 1f
	NEXT_BLOCK_INSTR
1:	TAKE_BRANCH \check
.endm
// FasterFloatType: func @4 (bit 0: double), rd @5, rs2 @6, rs1 @7
// Without NaN-boxing a single-precision result only writes the low half.
.macro FP_ARITH op
	F_B7 x0
	F_B6 x1
	F_B5 x2
	FREG_ADDR x0, x0
	FREG_ADDR x1, x1
	FREG_ADDR x2, x2
	tbnz x9, #32, 1f
	ldr s0, [x0, #FPOFF]
	ldr s1, [x1, #FPOFF]
	\op s0, s0, s1
	str s0, [x2, #FPOFF]
	NEXT_INSTR
1:	ldr d0, [x0, #FPOFF]
	ldr d1, [x1, #FPOFF]
	\op d0, d0, d1
	str d0, [x2, #FPOFF]
	NEXT_INSTR
.endm
// RVC: REG(dst @7) = REG(dst) \op REG(src @6)
.macro C_ARITH op
	F_B6 x1
	F_B7 x0
	LDREG T1, x1
	LDREG T2, x0
	\op T2, T2, T1
	STREG T2, x0
	NEXT_C_INSTR
.endm
// RVC: REG(dst @7) = REG(dst) \op imm16 (@4)
.macro C_IMM op, signed=1
	F_B7 x0
.if \signed
	F_S4 x1
.else
	F_U4 x1
.endif
	LDREG T2, x0
	\op T2, T2, T1
	STREG T2, x0
	NEXT_C_INSTR
.endm

// -== Handler lists ==-
// The handlers this file defines. The lists must match ASMDISPATCH_*HANDLERS
// in amd64/asm_dispatch.cpp.
#define HANDLERS(X) \
	X(fallback) X(stop) \
	X(addi) X(li) X(mv) X(slli) X(srli) X(srai) X(slti) X(sltiu) \
	X(xori) X(ori) X(andi) X(sext_b) X(sext_h) X(bseti) X(bexti) \
	X(lui) X(auipc) \
	X(op_add) X(op_sub) X(op_sll) X(op_srl) X(op_sra) X(op_slt) X(op_sltu) \
	X(op_xor) X(op_or) X(op_and) X(op_mul) X(op_zext_h) \
	X(op_div) X(op_divu) X(op_rem) X(op_remu) \
	X(op_sh1add) X(op_sh2add) X(op_sh3add) \
	X(function) X(funcblock) X(syscall) X(dyncall) X(system) X(step) \
	X(translator) X(asmjit) X(livepatch) \
	X(beq) X(bne) X(beq_fw) X(bne_fw) X(blt) X(bge) X(bltu) X(bgeu) \
	X(jal) X(fast_jal) X(fast_call) X(jalr) \
	X(ldw) X(ldh) X(ldhu) X(ldb) X(ldbu) X(stw) X(sth) X(stb) \
	X(flw) X(fld) X(fsw) X(fsd) X(fadd) X(fsub) X(fmul) X(fdiv) X(fmadd)
#define RV64_HANDLERS(X) \
	X(addiw) X(slliw) X(srliw) X(sraiw) \
	X(op_addw) X(op_subw) X(op_mulw) X(op_add_uw) X(op_sh1add_uw) X(op_sh2add_uw) \
	X(ldwu) X(ldd) X(std)
#define C_HANDLERS(X) \
	X(c_addi) X(c_mv) X(c_slli) X(c_srli) X(c_andi) X(c_add) X(c_xor) X(c_or) \
	X(c_bnez) X(c_beqz) X(c_jmp) X(c_jal_addiw) X(c_jr) X(c_jalr) \
	X(c_function) X(c_ldw) X(c_stw) X(c_lui) X(c_fld) X(c_fsd) X(c_step) \
	X(c_srai) X(c_sub) X(c_and)
#define RV32_C_HANDLERS(X) X(c_flw) X(c_fsw)
#define RV64_C_HANDLERS(X) X(c_ldd) X(c_std) X(c_subw) X(c_addw)

// The groups of fused_pairs.inc that exist in this configuration
#define FUSED_IF_COMMON(...) __VA_ARGS__
#if XLEN == 64
#define FUSED_IF_RV64(...) __VA_ARGS__
#else
#define FUSED_IF_RV64(...)
#endif
#if COMPRESSED
#define FUSED_IF_C(...) __VA_ARGS__
#else
#define FUSED_IF_C(...)
#endif
#if COMPRESSED && XLEN == 32
#define FUSED_IF_C_RV32(...) __VA_ARGS__
#else
#define FUSED_IF_C_RV32(...)
#endif
#if COMPRESSED && XLEN == 64
#define FUSED_IF_C_RV64(...) __VA_ARGS__
#else
#define FUSED_IF_C_RV64(...)
#endif

.macro HANDLER name
	.p2align 4
h_\name:
.endm

	.text

// uint32_t <prefix>dispatch(AsmDispatchContext* ctx)
// Starts executing at ctx->pc, which must be inside the execute segment.
// Returns an EXIT_* code with ctx->pc set accordingly. The frame does not
// change after the prologue, so the CFI of the prologue covers every handler
// after it, up to .cfi_endproc at the end of the file.
	.p2align 4
	.globl SYM(dispatch)
	.type SYM(dispatch), %function
SYM(dispatch):
	.cfi_startproc
	stp x29, x30, [sp, #-FRAME]!
	.cfi_def_cfa_offset FRAME
	.cfi_offset x29, -FRAME
	.cfi_offset x30, -FRAME+8
	mov x29, sp
	stp x19, x20, [sp, #16]
	stp x21, x22, [sp, #32]
	stp x23, x24, [sp, #48]
	stp x25, x26, [sp, #64]
	stp x27, x28, [sp, #80]
	.cfi_offset x19, -FRAME+16
	.cfi_offset x20, -FRAME+24
	.cfi_offset x21, -FRAME+32
	.cfi_offset x22, -FRAME+40
	.cfi_offset x23, -FRAME+48
	.cfi_offset x24, -FRAME+56
	.cfi_offset x25, -FRAME+64
	.cfi_offset x26, -FRAME+72
	.cfi_offset x27, -FRAME+80
	.cfi_offset x28, -FRAME+88
	str x0, [sp, #CTX_SLOT]
	ldr RREGS,  [x0, #CTX_REGS]
	ldr RDEC,   [x0, #CTX_DECODER]
	ldr RARENA, [x0, #CTX_ARENA]
	ldr RTAB,   [x0, #CTX_TABLE]
	LOAD_ARENA_BOUNDS x0
#if COUNTING
	ldr RMAX,   [x0, #CTX_MAX]
	ldr RCOUNT, [x0, #CTX_COUNTER]
#else
	ldr RBEGIN, [x0, #CTX_BEGIN]
	ldr RLEN,   [x0, #CTX_LENGTH]
#endif
	ldr T1, [x0, #CTX_PC]
#if XLEN == 64
	add RENT, RDEC, x1, lsl #PC2DEC
#else
	add RENT, RDEC, w1, uxtw #PC2DEC
#endif
#if COUNTING
	ldrb w2, [x0, #CTX_ENTER_COUNT]
	cbz w2, 1f
	DISPATCH_BLOCK
1:
#endif
	DISPATCH

// Leave the dispatch: w0 = exit code, ctx->pc already written
exit_dispatch:
#if COUNTING
	ldr x1, [sp, #CTX_SLOT]
	str RCOUNT, [x1, #CTX_COUNTER]
	str RMAX, [x1, #CTX_MAX]
#endif
	.cfi_remember_state
	ldp x19, x20, [sp, #16]
	ldp x21, x22, [sp, #32]
	ldp x23, x24, [sp, #48]
	ldp x25, x26, [sp, #64]
	ldp x27, x28, [sp, #80]
	ldp x29, x30, [sp], #FRAME
	.cfi_restore x19
	.cfi_restore x20
	.cfi_restore x21
	.cfi_restore x22
	.cfi_restore x23
	.cfi_restore x24
	.cfi_restore x25
	.cfi_restore x26
	.cfi_restore x27
	.cfi_restore x28
	.cfi_restore x29
	.cfi_restore x30
	.cfi_def_cfa_offset 0
	ret
	.cfi_restore_state

#if COUNTING
// The counter reached the max: stop at the pc of x19, or at x0
exit_overflow_rbx:
	RECONSTRUCT_PC x0
exit_overflow:
	ldr x1, [sp, #CTX_SLOT]
	str T0, [x1, #CTX_PC]
	mov w0, #EXIT_OVERFLOW
	b exit_dispatch
#endif

// Jump to x0, which is outside the current execute segment
exit_jump:
	ldr x1, [sp, #CTX_SLOT]
	str T0, [x1, #CTX_PC]
	mov w0, #EXIT_JUMP
	b exit_dispatch

do_stop:
	RECONSTRUCT_PC x0
	add T0, T0, #4
	ldr x1, [sp, #CTX_SLOT]
	str T0, [x1, #CTX_PC]
	mov w0, #EXIT_STOP
	b exit_dispatch

// <prefix>call asked to continue at ctx->pc
call_goto:
	ldr x1, [sp, #CTX_SLOT]
	ldr T0, [x1, #CTX_PC]
	JUMP_TO_EAX

// A fast path could not handle the current instruction (a load or store
// outside the flat arena, a misaligned jump, ...): C++ executes it from
// memory (CPU::step_one), raising whatever exception is due.
do_fallback:
	CALL_CPP KIND_STEP, NEXT_INSTR
#if COMPRESSED
do_c_fallback:
	CALL_CPP KIND_C_STEP, NEXT_C_INSTR
#endif

// RV32I_BC_INVALID and every unknown bytecode
.macro BODY_fallback
	CALL_CPP KIND_INVALID, NEXT_INSTR
.endm
.macro BODY_stop
	b do_stop
.endm

// -== Immediate ALU, FasterItype: imm16 @4, rs2 (src) @6, rs1 (dst) @7 ==-

.macro BODY_addi
	IMM_ARITH add
.endm
// FasterImmediate: rd @4, imm16 @6
.macro BODY_li
	F_B4 x0
	F_S6 x2
	STREG T2, x0
	NEXT_INSTR
.endm
// FasterMove: rs1 @4, rd @5
.macro BODY_mv
	F_B4 x1
	F_B5 x0
	LDREG T1, x1
	STREG T1, x0
	NEXT_INSTR
.endm
.macro BODY_slli
	IMM_ARITH lsl, 0
.endm
.macro BODY_srli
	IMM_ARITH lsr, 0
.endm
.macro BODY_srai
	IMM_ARITH asr, 0
.endm
.macro BODY_slti
	ITYPE
	cmp T1, T2
	cset T1, lt
	STREG T1, x0
	NEXT_INSTR
.endm
.macro BODY_sltiu
	ITYPE
	cmp T1, T2
	cset T1, lo
	STREG T1, x0
	NEXT_INSTR
.endm
.macro BODY_xori
	IMM_ARITH eor
.endm
.macro BODY_ori
	IMM_ARITH orr
.endm
.macro BODY_andi
	IMM_ARITH and
.endm
.macro BODY_sext_b
	ITYPE
	sxtb T1, w1
	STREG T1, x0
	NEXT_INSTR
.endm
.macro BODY_sext_h
	ITYPE
	sxth T1, w1
	STREG T1, x0
	NEXT_INSTR
.endm
.macro BODY_bseti
	ITYPE 0
	mov T3, #1
	lsl T3, T3, T2
	orr T1, T1, T3
	STREG T1, x0
	NEXT_INSTR
.endm
.macro BODY_bexti
	ITYPE 0
	lsr T1, T1, T2
	and T1, T1, #1
	STREG T1, x0
	NEXT_INSTR
.endm

// -== Upper immediates, FasterJtype: offset24 @4..6, rd @7 ==-

.macro BODY_lui
	lsr x0, x9, #32
	F_B7 x1
	lsl w0, w0, #8
#if XLEN == 64
	sxtw x0, w0
#endif
	STREG T0, x1
	NEXT_INSTR
.endm
.macro BODY_auipc
	lsr x0, x9, #32
	F_B7 x1
	lsl w0, w0, #8
#if XLEN == 64
	sxtw x0, w0
#endif
	RECONSTRUCT_PC x2
	add T0, T0, T2
	STREG T0, x1
	NEXT_INSTR
.endm

// -== Register ALU, FasterOpType: rd16 @4, rs2 @6, rs1 @7 ==-

.macro BODY_op_add
	OP_ARITH add
.endm
.macro BODY_op_sub
	OP_ARITH sub
.endm
.macro BODY_op_sll
	OP_ARITH lsl
.endm
.macro BODY_op_srl
	OP_ARITH lsr
.endm
.macro BODY_op_sra
	OP_ARITH asr
.endm
.macro BODY_op_slt
	OPTYPE
	cmp T2, T1
	cset T3, lt
	STREG T3, x0
	NEXT_INSTR
.endm
.macro BODY_op_sltu
	OPTYPE
	cmp T2, T1
	cset T3, lo
	STREG T3, x0
	NEXT_INSTR
.endm
.macro BODY_op_xor
	OP_ARITH eor
.endm
.macro BODY_op_or
	OP_ARITH orr
.endm
.macro BODY_op_and
	OP_ARITH and
.endm
.macro BODY_op_mul
	OP_ARITH mul
.endm
// Division with RISC-V semantics. AArch64 does not trap either: x/0 = 0,
// which becomes all ones here, and MIN/-1 = MIN, as RISC-V wants. The
// remainder a - (a/b)*b is then already right in every case: a for b = 0,
// and 0 for MIN/-1.
.macro BODY_op_div
	OPTYPE
	sdiv T3, T2, T1
	cmp T1, #0
	csinv T3, T3, TZR, ne
	STREG T3, x0
	NEXT_INSTR
.endm
.macro BODY_op_divu
	OPTYPE
	udiv T3, T2, T1
	cmp T1, #0
	csinv T3, T3, TZR, ne
	STREG T3, x0
	NEXT_INSTR
.endm
.macro BODY_op_rem
	OPTYPE
	sdiv T3, T2, T1
	msub T3, T3, T1, T2
	STREG T3, x0
	NEXT_INSTR
.endm
.macro BODY_op_remu
	OPTYPE
	udiv T3, T2, T1
	msub T3, T3, T1, T2
	STREG T3, x0
	NEXT_INSTR
.endm
.macro BODY_op_zext_h
	OPTYPE
	uxth w3, w2
	STREG T3, x0
	NEXT_INSTR
.endm
// rd = rs2 + (rs1 << n)
.macro BODY_op_sh1add
	OPTYPE
	add T3, T1, T2, lsl #1
	STREG T3, x0
	NEXT_INSTR
.endm
.macro BODY_op_sh2add
	OPTYPE
	add T3, T1, T2, lsl #2
	STREG T3, x0
	NEXT_INSTR
.endm
.macro BODY_op_sh3add
	OPTYPE
	add T3, T1, T2, lsl #3
	STREG T3, x0
	NEXT_INSTR
.endm

#if XLEN == 64
// -== RV64 32-bit operations, sign-extended to 64 bits ==-

.macro BODY_addiw
	ITYPE
	add w1, w1, w2
	sxtw x1, w1
	STREG x1, x0
	NEXT_INSTR
.endm
.macro BODY_slliw
	ITYPE 0
	lsl w1, w1, w2
	sxtw x1, w1
	STREG x1, x0
	NEXT_INSTR
.endm
.macro BODY_srliw
	ITYPE 0
	lsr w1, w1, w2
	sxtw x1, w1
	STREG x1, x0
	NEXT_INSTR
.endm
.macro BODY_sraiw
	ITYPE 0
	asr w1, w1, w2
	sxtw x1, w1
	STREG x1, x0
	NEXT_INSTR
.endm
.macro BODY_op_addw
	OPTYPE
	add w3, w2, w1
	sxtw x3, w3
	STREG x3, x0
	NEXT_INSTR
.endm
.macro BODY_op_subw
	OPTYPE
	sub w3, w2, w1
	sxtw x3, w3
	STREG x3, x0
	NEXT_INSTR
.endm
.macro BODY_op_mulw
	OPTYPE
	mul w3, w2, w1
	sxtw x3, w3
	STREG x3, x0
	NEXT_INSTR
.endm
// rd = rs2 + (zext32(rs1) << n)
.macro BODY_op_add_uw
	OPTYPE
	add x3, x1, w2, uxtw
	STREG x3, x0
	NEXT_INSTR
.endm
.macro BODY_op_sh1add_uw
	OPTYPE
	add x3, x1, w2, uxtw #1
	STREG x3, x0
	NEXT_INSTR
.endm
.macro BODY_op_sh2add_uw
	OPTYPE
	add x3, x1, w2, uxtw #2
	STREG x3, x0
	NEXT_INSTR
.endm
#endif // XLEN == 64

// -== Calls into C++ ==-

.macro BODY_function
	CALL_CPP KIND_FUNCTION, NEXT_INSTR
.endm
.macro BODY_funcblock
	CALL_CPP KIND_FUNCBLOCK, NEXT_INSTR   // C++ always continues at ctx->pc
.endm
.macro BODY_syscall
	CALL_CPP KIND_SYSCALL, NEXT_BLOCK_INSTR
.endm
.macro BODY_dyncall
	CALL_CPP KIND_DYNCALL, NEXT_INSTR
.endm
.macro BODY_system
	CALL_CPP KIND_SYSTEM, NEXT_BLOCK_INSTR
.endm
.macro BODY_translator
	CALL_CPP KIND_TRANSLATOR, NEXT_INSTR
.endm
.macro BODY_asmjit
	CALL_CPP KIND_ASMJIT, NEXT_INSTR
.endm
.macro BODY_livepatch
	CALL_CPP KIND_LIVEPATCH, NEXT_INSTR
.endm
.macro BODY_step
	CALL_CPP KIND_STEP, NEXT_INSTR
.endm

// -== Branches, FasterItype: imm16 @4, rs2 @6, rs1 @7 ==-

.macro BODY_beq
	BRANCH eq, 1
.endm
.macro BODY_bne
	BRANCH ne, 1
.endm
.macro BODY_beq_fw
	BRANCH eq, 0
.endm
.macro BODY_bne_fw
	BRANCH ne, 0
.endm
.macro BODY_blt
	BRANCH lt, 1
.endm
.macro BODY_bge
	BRANCH ge, 1
.endm
.macro BODY_bltu
	BRANCH lo, 1
.endm
.macro BODY_bgeu
	BRANCH hs, 1
.endm

// -== Jumps ==-

// FasterJtype: rd = pc + 4, pc += sext(offset24)
.macro BODY_jal
	RECONSTRUCT_PC x0
	F_B7 x1
	add T0, T0, #4
	STREG T0, x1
	sbfx x2, x9, #32, #24
	add RENT, RENT, x2, lsl #PC2DEC
	CHECK_OVERFLOW_RBX
	DISPATCH_BLOCK
.endm
.macro BODY_fast_jal
	asr x2, x9, #32
	add RENT, RENT, x2, lsl #PC2DEC
	CHECK_OVERFLOW_RBX
	DISPATCH_BLOCK
.endm
.macro BODY_fast_call
	RECONSTRUCT_PC x0
	add T0, T0, #4
	str T0, [RREGS, #XB]
	asr x2, x9, #32
	add RENT, RENT, x2, lsl #PC2DEC
	CHECK_OVERFLOW_RBX
	DISPATCH_BLOCK
.endm
// FasterItype: target = REG(rs2) + imm, rs1 = link register (0 = none)
.macro BODY_jalr
	F_B6 x1
	F_S4 x2
	LDREG T1, x1
	add T0, T1, T2
	and T0, T0, #~1
#if COMPRESSED == 0
	tst w0, #3
	b.ne do_fallback        // C++ raises the misaligned exception
#endif
	F_B7 x1
	cbz x1, 2f
	RECONSTRUCT_PC x2
	add T2, T2, #4
	STREG T2, x1
2:	JUMP_TO_EAX
.endm

// -== Loads and stores ==-

#if XLEN == 64
.macro BODY_ldw
	LOAD ldrsw, x3
.endm
.macro BODY_ldh
	LOAD ldrsh, x3
.endm
.macro BODY_ldb
	LOAD ldrsb, x3
.endm
.macro BODY_ldwu
	LOAD ldr, w3
.endm
.macro BODY_ldd
	LOAD ldr, x3
.endm
.macro BODY_std
	STORE str, x4
.endm
#else
.macro BODY_ldw
	LOAD ldr, w3
.endm
.macro BODY_ldh
	LOAD ldrsh, w3
.endm
.macro BODY_ldb
	LOAD ldrsb, w3
.endm
#endif
.macro BODY_ldhu
	LOAD ldrh, w3
.endm
.macro BODY_ldbu
	LOAD ldrb, w3
.endm
.macro BODY_stw
	STORE str, w4
.endm
.macro BODY_sth
	STORE strh, w4
.endm
.macro BODY_stb
	STORE strb, w4
.endm

.macro BODY_flw
	FLOAD s0
.endm
.macro BODY_fld
	FLOAD d0
.endm
.macro BODY_fsw
	FSTORE s0
.endm
.macro BODY_fsd
	FSTORE d0
.endm

// -== Floating point arithmetic (C++ maps these only without FCSR emulation
//     and NaN-boxing) ==-

.macro BODY_fadd
	FP_ARITH fadd
.endm
.macro BODY_fsub
	FP_ARITH fsub
.endm
.macro BODY_fmul
	FP_ARITH fmul
.endm
.macro BODY_fdiv
	FP_ARITH fdiv
.endm
// The original instruction (R4type): rd @7, rs1 @15, rs2 @20, funct2 @25,
// rs3 @27. rd = rs1 * rs2 + rs3, rounded once.
.macro BODY_fmadd
	lsr x0, x9, #32
	tst w0, #(1 << 26)      // funct2 >= 2 (half, quad)
	b.ne do_fallback
	ubfx x1, x0, #15, #5
	ubfx x2, x0, #20, #5
	ubfx x3, x0, #27, #5
	ubfx x4, x0, #7, #5
	FREG_ADDR x1, x1
	FREG_ADDR x2, x2
	FREG_ADDR x3, x3
	FREG_ADDR x4, x4
	tbnz w0, #25, 1f
	ldr s1, [x1, #FPOFF]
	ldr s2, [x2, #FPOFF]
	ldr s3, [x3, #FPOFF]
	fmadd s0, s1, s2, s3
	str s0, [x4, #FPOFF]
	NEXT_INSTR
1:	ldr d1, [x1, #FPOFF]
	ldr d2, [x2, #FPOFF]
	ldr d3, [x3, #FPOFF]
	fmadd d0, d1, d2, d3
	str d0, [x4, #FPOFF]
	NEXT_INSTR
.endm

#if COMPRESSED
// -== Compressed bytecodes, one decoder entry each ==-

// FasterItype: rs1 = dst @7, rs2 = src @6, imm16 @4
.macro BODY_c_addi
	ITYPE
	add T1, T1, T2
	STREG T1, x0
	NEXT_C_INSTR
.endm
.macro BODY_c_mv
	F_B4 x1
	F_B5 x0
	LDREG T1, x1
	STREG T1, x0
	NEXT_C_INSTR
.endm
.macro BODY_c_slli
	C_IMM lsl, 0
.endm
.macro BODY_c_srli
	C_IMM lsr, 0
.endm
.macro BODY_c_srai
	C_IMM asr, 0
.endm
.macro BODY_c_andi
	C_IMM and
.endm
.macro BODY_c_sub
	C_ARITH sub
.endm
.macro BODY_c_and
	C_ARITH and
.endm
.macro BODY_c_add
	C_ARITH add
.endm
.macro BODY_c_xor
	C_ARITH eor
.endm
.macro BODY_c_or
	C_ARITH orr
.endm
#if XLEN == 64
.macro BODY_c_subw
	F_B6 x1
	F_B7 x0
	LDREG x1, x1
	LDREG x2, x0
	sub w2, w2, w1
	sxtw x2, w2
	STREG x2, x0
	NEXT_C_INSTR
.endm
.macro BODY_c_addw
	F_B6 x1
	F_B7 x0
	LDREG x1, x1
	LDREG x2, x0
	add w2, w2, w1
	sxtw x2, w2
	STREG x2, x0
	NEXT_C_INSTR
.endm
#endif

// Taken compressed branches check the counter in either direction, like
// PERFORM_BRANCH in cpu_dispatch.cpp
.macro BODY_c_bnez
	F_B7 x0
	LDREG T0, x0
	cbnz T0, 1f
	NEXT_BLOCK_C_INSTR
1:	TAKE_BRANCH 1
.endm
.macro BODY_c_beqz
	F_B7 x0
	LDREG T0, x0
	cbz T0, 1f
	NEXT_BLOCK_C_INSTR
1:	TAKE_BRANCH 1
.endm
.macro BODY_c_jmp
	TAKE_BRANCH 1
.endm
.macro BODY_c_jal_addiw
#if XLEN == 64
	// C.ADDIW: rd = sext32(rd + imm)
	F_B7 x0
	F_S4 x1
	LDREG x2, x0
	add w2, w2, w1
	sxtw x2, w2
	STREG x2, x0
	NEXT_C_INSTR
#else
	// C.JAL
	RECONSTRUCT_PC x0
	add w0, w0, #2
	str w0, [RREGS, #XB]
	TAKE_BRANCH 1
#endif
.endm
// instr = register index
.macro BODY_c_jr
	lsr x1, x9, #32
	LDREG T0, x1
	and T0, T0, #~1
	JUMP_TO_EAX
.endm
.macro BODY_c_jalr
	lsr x1, x9, #32
	LDREG T0, x1
	and T0, T0, #~1
	RECONSTRUCT_PC x2
	add T2, T2, #2
	str T2, [RREGS, #XB]
	JUMP_TO_EAX
.endm
// FasterJtype: offset24 @4..6, rd @7
.macro BODY_c_lui
	lsr x0, x9, #32
	F_B7 x1
	lsl w0, w0, #8
#if XLEN == 64
	sxtw x0, w0
#endif
	STREG T0, x1
	NEXT_C_INSTR
.endm

.macro BODY_c_fld
	FLOAD d0, NEXT_C_INSTR, do_c_fallback
.endm
.macro BODY_c_fsd
	FSTORE d0, NEXT_C_INSTR, do_c_fallback
.endm
#if XLEN == 32
.macro BODY_c_flw
	FLOAD s0, NEXT_C_INSTR, do_c_fallback
.endm
.macro BODY_c_fsw
	FSTORE s0, NEXT_C_INSTR, do_c_fallback
.endm
#endif
.macro BODY_c_step
	CALL_CPP KIND_C_STEP, NEXT_C_INSTR
.endm
// RV32C_BC_FUNCTION keeps the original 16-bit instruction, executed by C++
.macro BODY_c_function
	CALL_CPP KIND_C_FUNCTION, NEXT_C_INSTR
.endm
#if XLEN == 64
.macro BODY_c_ldw
	LOAD ldrsw, x3, NEXT_C_INSTR, do_c_fallback
.endm
.macro BODY_c_ldd
	LOAD ldr, x3, NEXT_C_INSTR, do_c_fallback
.endm
.macro BODY_c_std
	STORE str, x4, NEXT_C_INSTR, do_c_fallback
.endm
#else
.macro BODY_c_ldw
	LOAD ldr, w3, NEXT_C_INSTR, do_c_fallback
.endm
#endif
.macro BODY_c_stw
	STORE str, w4, NEXT_C_INSTR, do_c_fallback
.endm
#endif // COMPRESSED

// -== The handlers ==-
// Every handler above is a BODY_<name> macro, emitted here in list order
#define EMIT(name) HANDLER name; BODY_##name;
HANDLERS(EMIT)
#if XLEN == 64
RV64_HANDLERS(EMIT)
#endif
#if COMPRESSED
C_HANDLERS(EMIT)
#if XLEN == 64
RV64_C_HANDLERS(EMIT)
#else
RV32_C_HANDLERS(EMIT)
#endif
#endif

// -== Fused pairs ==-
// The decoder gives an entry the bytecode of a pair (fused_pairs.inc, shared
// with C++) when it is the first half and the next instruction, in the same
// block, is the second half. The handler for the pair is the body of the
// first half, which continues with the handler of the second half (from the
// next entry, which is unchanged) by a direct branch instead of a dispatch.
// The straight-line continuations, NEXT_INSTR and NEXT_C_INSTR, are redefined
// for the body; everything else (branches, fallbacks into C++) dispatches.
.macro FUSED name, a, b
	HANDLER f_\name
	.purgem NEXT_INSTR
	.purgem NEXT_C_INSTR
	.macro NEXT_INSTR
	ldr x9, [RENT, #I4]!
	b h_\b
	.endm
	.macro NEXT_C_INSTR
	ldr x9, [RENT, #I2]!
	b h_\b
	.endm
	BODY_\a
	.purgem NEXT_INSTR
	.purgem NEXT_C_INSTR
	.macro NEXT_INSTR
	NEXT I4
	.endm
	.macro NEXT_C_INSTR
	NEXT I2
	.endm
.endm
#define FUSED_PAIR(name, a, b, A, B, G) FUSED_IF_##G(FUSED name, a, b;)
#include "fused_pairs.inc"
#undef FUSED_PAIR

	.cfi_endproc
	.size SYM(dispatch), . - SYM(dispatch)

// -== Exported handler arrays ==-
// Each <prefix><name> is an array with the address of every copy of the
// handler, of which there is one (<prefix>ncopies).
#define EXPORT_AS(sym, handler) .globl SYM(sym); .type SYM(sym), %object; SYM(sym): .quad h_##handler;
#define EXPORT(name) EXPORT_AS(name, name)


	.section .data.rel.ro, "aw"
	.p2align 3
HANDLERS(EXPORT)
#if XLEN == 64
RV64_HANDLERS(EXPORT)
#endif
#if COMPRESSED
C_HANDLERS(EXPORT)
#if XLEN == 64
RV64_C_HANDLERS(EXPORT)
#else
RV32_C_HANDLERS(EXPORT)
#endif
#endif
#define FUSED_PAIR(name, a, b, A, B, G) FUSED_IF_##G(EXPORT(f_##name))
#include "fused_pairs.inc"
#undef FUSED_PAIR

	.section .rodata
	.p2align 2
	.globl SYM(ncopies)
	.type SYM(ncopies), %object
SYM(ncopies):
	.word 1

	.section .note.GNU-stack, "", %progbits
