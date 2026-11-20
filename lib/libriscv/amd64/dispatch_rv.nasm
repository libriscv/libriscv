;; RV32 and RV64 dispatch for libriscv, AMD64 SysV and Win64 (MinGW).
;;
;; Assemble once per register width and mode:
;;   nasm -f elf64 -DXLEN=32|64 -DCOUNTING=0|1 dispatch_rv.nasm
;; or with -f win64 for Windows. The Win64 differences (calling convention,
;; stack frame, sections and unwind info) are all under %if WIN64, and the
;; SysV output does not depend on them.
;; COUNTING=0 is the inaccurate dispatch (CPU::simulate_inaccurate), with the
;; exported symbols prefixed rv32asm_ or rv64asm_. COUNTING=1 counts
;; instructions like cpu_dispatch.cpp (CPU::simulate), prefixed rv32asmc_ or
;; rv64asmc_. See "Instruction counting" below. -DCOMPRESSED=1 must match
;; RISCV_EXT_C: the decoder cache then has one entry per 2 bytes of code.
;;
;; The bytecodes and the 8-byte decoder entries are produced by the regular
;; decoder (decode_bytecodes.cpp) and are shared with the C++ dispatch:
;;   uint8_t  bytecode, uint8_t handler, uint16_t idxend, uint32_t instr
;; or with the C-extension, where blocks are not a multiple of 4 bytes:
;;   uint8_t  bytecode, uint8_t handler, uint8_t idxend, uint8_t icount, uint32_t instr
;;
;; The dispatch table (bytecode -> handler address) is filled in by C++ from the
;; real enum values, so this file does not depend on which extensions are enabled.
;; Every bytecode without a handler here maps to the fallback handler, which
;; returns to C++ so that it can execute exactly one instruction and re-enter.
;;
;; The only call out is to <prefix>call (system calls, FUNCTION entries, native
;; code, and the fallback for anything the fast paths here do not handle), which
;; is noexcept and hands any exception back to C++, so nothing unwinds through
;; these frames. Every bytecode has a handler, so the dispatch only returns to
;; C++ to stop, to change execute segment or to throw.
;;
;; Requires BMI2 (rorx), AVX and FMA3. C++ checks for them at run-time.
;;
;; What bounds an interpreter like this on Zen 4 (fib, ~2.4 host cycles per
;; guest instruction, 2.3 G instr/s, from 4.2 for the C++ dispatch):
;;  - Op count. At the end it retires ~5 ops/cycle out of 6, so every op counts:
;;    jmp [mem] and load-op ALU forms are 2 ops each, rorx is 1 op but 2/cycle.
;;  - Memory ops (3/cycle). The 8-byte entry is loaded once, by the previous
;;    handler's dispatch, and fields are extracted from it in registers where
;;    that is cheaper than a byte load (see NEXT).
;;  - The decoder pointer chain. A taken branch loads its offset and adds it to
;;    rbx, and every later load waits for that. See the backward branch table.
;;  - Per-address predictors. See handler replication.
;;  - Fetch. Every handler starts on a 64-byte boundary and should fit in one
;;    64-byte window: each handler ends in a taken indirect jump, and one that
;;    straddles two windows costs another fetch cycle. Going from 16- to
;;    64-byte alignment was worth +16% CoreMark, -13% Dhrystone cycles and
;;    -20% STREAM (rv32gb) cycles, which had been ~37% front-end bound. Hence
;;    the choice of base registers (see RREGS) and the short fallback jumps.
;; The register file in memory is not the problem: with memory renaming a
;; store and a dependent load cost ~1-2 cycles, once the predictor can tell
;; guest instructions apart.
[bits 64]
DEFAULT REL

%ifndef XLEN
%define XLEN 32
%endif
%ifndef COMPRESSED
%define COMPRESSED 0
%endif
%ifndef COUNTING
%define COUNTING 0
%endif

%ifidn __?OUTPUT_FORMAT?__, win64
%define WIN64 1
%else
%define WIN64 0
%endif

;; Stack frame. SysV: the context is at [rsp], and CALL_CPP pushes the pinned
;; caller-saved registers around the call. Win64: rsi and rdi are callee-saved,
;; and every call needs 32 bytes of shadow space below the return address,
;; which the callee may overwrite. So the frame is allocated once, in the
;; prologue, and rsp does not move in the handlers (which also makes one set
;; of unwind codes valid for all of them):
;;   [rsp + 0..31] shadow space, [rsp + 32] saved r11, [rsp + 40] context
%if WIN64
%define CTX_SLOT   [rsp + 40]
%define R11_SLOT   [rsp + 32]
%define FRAME_SIZE 56       ; 8 pushes + 56 = 16-byte aligned at calls
%define RELRO_SECTION .rdata rdata align=8
%define RODATA_SECTION .rdata rdata align=8
%else
%define CTX_SLOT   [rsp]
%define RELRO_SECTION .data.rel.ro
%define RODATA_SECTION .rodata
%endif

%if COUNTING
%xdefine PFX rv %+ XLEN %+ asmc_
%else
%xdefine PFX rv %+ XLEN %+ asm_
%endif

;; Register width. x-prefixed names are the host registers that hold a guest
;; register value (or a guest address) of XLEN bits.
%if XLEN == 64
%define XB    8
%define XW    qword
%define xax   rax
%define xcx   rcx
%define xdx   rdx
%define xdi   rdi
%define xr8   r8
%define xr9   r9
%define xr10  r10
%define xr11  r11
%define xr12  r12
%define FPOFF 272   ; offsetof(Registers<8>, m_regfl)
%elif XLEN == 32
%define XB    4
%define XW    dword
%define xax   eax
%define xcx   ecx
%define xdx   edx
%define xdi   edi
%define xr8   r8d
%define xr9   r9d
%define xr10  r10d
%define xr11  r11d
%define xr12  r12d
%define FPOFF 136   ; offsetof(Registers<4>, m_regfl)
%else
%error "XLEN must be 32 or 64"
%endif

%if COMPRESSED
%define SHIFT 1
%else
%define SHIFT 2
%endif
;; One decoder entry (8 bytes) per 2^SHIFT bytes of code.
%define I4      (8 * (4 >> SHIFT))  ; decoder bytes for a 4-byte instruction
%define I2      8                   ; decoder bytes for a 2-byte instruction
%define PC2DEC  (3 - SHIFT)         ; decoder offset = pc << PC2DEC
%define BRSCALE (1 << PC2DEC)       ; branch: decoder += imm * BRSCALE

;; Pinned state
;;   rbx    decoder entry pointer (current instruction)
;;   RREGS  register file: x0..x31 (XB bytes each), then pc, fcsr, f0..f31
;;   RDEC   decoder cache base (indexed by pc >> SHIFT)
;;   RARENA flat arena base
;;   r14    dispatch tables (NCOPIES x 256 x 8-byte absolute addresses)
;;   RBTAB  end of the backward-branch table (entry for offset 0)
;; Handlers should fit in 64 bytes (one fetch window), so the bases that
;; nearly every handler addresses are the cheapest to encode: the register
;; file is in rsi (no REX prefix, short VEX form), the arena in r15. rbp and
;; r13 as a base always take a displacement byte, so they hold what the hot
;; paths do not use as a base.
%define RREGS  rsi
%define RDEC   rbp
%define RARENA r15
%define RBTAB  r13
;;   CTX_SLOT context (see "Stack frame")
;;   xr8  arena read boundary   (addr - 0x1000 < xr8 is readable)
;;   xr9  initial rodata end
;;   xr10 arena write boundary  (addr - xr9 < xr10 is writable)
;;   xr11 current execute segment begin          (COUNTING: r11 = max counter)
;;   xr12 current execute segment length (end - begin)  (COUNTING: r12 = counter)
;; Scratch: rax, rcx, rdx, rdi, xmm0, xmm1
;;
;; Instruction counting (COUNTING=1) follows cpu_dispatch.cpp exactly, so that
;; both report the same counts and stop at the same places: the instruction
;; count of a whole block (idxend + 1, from its first decoder entry) is added
;; when a block is entered, and the counter is compared with the max only where
;; the C++ dispatch does it, which is before taking a branch that can go
;; backwards, before jumps, after calls into C++ and when re-entering. The
;; segment bounds, which only JALR needs, are then read from the context.

;; Context layout (struct AsmDispatchContext in asm_dispatch.cpp)
%define CTX_REGS     0
%define CTX_DECODER  8
%define CTX_BEGIN    16
%define CTX_LENGTH   24
%define CTX_ARENA    32
%define CTX_RBOUND   40
%define CTX_RODATA   48
%define CTX_WBOUND   56
%define CTX_TABLE    64
%define CTX_PC       72
%define CTX_CPU      80
%define CTX_COUNTER  88
%define CTX_MAX      96
%define CTX_ENTER_COUNT 104   ; byte: count the block that the dispatch enters

;; Exit codes (returned to C++), also returned by <prefix>call
%define EXIT_STOP      0   ; STOP, or the machine was stopped (inaccurate)
%define EXIT_REENTER   1   ; continue at ctx->pc, without counting a new block
%define EXIT_JUMP      2   ; continue at ctx->pc, possibly in another segment
%define EXIT_EXCEPTION 3
;; Only returned by <prefix>call: continue with the next instruction, or
;; continue at ctx->pc (which may be outside the execute segment)
%define CALL_NEXT      4
%define CALL_GOTO      5
%define EXIT_OVERFLOW  6   ; COUNTING: the counter reached the max, at ctx->pc

;; What <prefix>call should do
%define KIND_FUNCTION   0
%define KIND_C_FUNCTION 1
%define KIND_SYSCALL    2
%define KIND_DYNCALL    3
%define KIND_SYSTEM     4
%define KIND_FUNCBLOCK  5
%define KIND_STEP       6   ; read, decode and execute the instruction in C++
%define KIND_INVALID    7   ; RV32I_BC_INVALID (and any unknown bytecode)
%define KIND_TRANSLATOR 8   ; binary translation
%define KIND_ASMJIT     9
%define KIND_LIVEPATCH  10
%define KIND_C_STEP     11  ; KIND_STEP for a 2-byte instruction

;; uint32_t <prefix>call(AsmDispatchContext*, DecoderData* entry, uint32_t kind) noexcept
;; Executes the current decoder entry in C++. Returns CALL_NEXT or CALL_GOTO to
;; continue, otherwise an EXIT_* code with ctx->pc set. It also refreshes the
;; arena boundaries in the context, as they can change in any system call.
extern %[PFX]call

%define RWREAD_BEGIN 0x1000

;; Handler replication. Every handler is emitted NCOPIES times, and each copy
;; dispatches through one of NCOPIES tables (its "color", fixed at assembly
;; time). C++ fills table[color * 256 + bytecode] with copy `color` of the
;; handler for that bytecode. The copy that runs a guest instruction is thereby
;; selected by the handler that ran before it.
;;
;; Why: the CPU predicts memory dependencies, renames memory and predicts
;; indirect jumps per instruction address. With one shared handler, every
;; guest instruction of a kind is the same load, store and jump to the CPU.
;; In `fib` the two MVs in the loop (mv a4,a0 and mv a3,a4) shared one store
;; and one dispatch jump, which cost store-to-load interlocks and, depending on
;; the path, two indirect mispredictions per iteration.
;;
;; The color is the class of the handler (CLASS_COLORS), so the copy depends on
;; what kind of instruction came before: after control flow, after ALU, after
;; memory. That separates most neighbouring instructions of the same kind with
;; three copies. The alternative, COLOR_MUL != 0, hashes the copy index into the
;; color, which makes the copy depend on the entire bytecode path: that also
;; separates them, but in data-dependent control flow it is effectively random
;; per execution and multiplied CoreMark's branch mispredictions 25x, and more
;; copies cost op cache capacity (CoreMark got slower beyond 4).
;;
;; Class colors leave one weakness: a run of identical instructions (ld x4,
;; sd x4) runs the same copy, whose dispatch jump then targets itself a few
;; times and something else at the end. Zen 4 predicted those badly (the RV64
;; STREAM copy loop: ~20 indirect mispredictions per 11 instructions), and
;; neither alternating nor cycling the copies within a run fixed it. Memory
;; handlers therefore branch to themselves directly, see NEXT_SELF.
%ifndef NCOPIES
%define NCOPIES 3
%endif
%ifndef COLOR_MUL
%define COLOR_MUL 0
%endif
%ifndef NO_CLASS_COLORS
%define CLASS_COLORS
%endif
%ifndef HALIGN
%define HALIGN 64
%endif
;; Handler classes (used with CLASS_COLORS): 0 = control flow, 1 = ALU,
;; 2 = memory and everything else
%define CLASS_DEFAULT 1
%define CLASS_fallback 0
%define CLASS_stop 0
%define CLASS_translator 0
%define CLASS_asmjit 0
%define CLASS_livepatch 0
%define CLASS_function 2
%define CLASS_c_function 2
%define CLASS_funcblock 2
%define CLASS_syscall 2
%define CLASS_dyncall 2
%define CLASS_system 2
%define CLASS_step 2
%define CLASS_beq 0
%define CLASS_bne 0
%define CLASS_beq_fw 0
%define CLASS_bne_fw 0
%define CLASS_blt 0
%define CLASS_bge 0
%define CLASS_bltu 0
%define CLASS_bgeu 0
%define CLASS_jal 0
%define CLASS_fast_jal 0
%define CLASS_fast_call 0
%define CLASS_jalr 0
%define CLASS_c_bnez 0
%define CLASS_c_beqz 0
%define CLASS_c_jmp 0
%if XLEN == 32
%define CLASS_c_jal_addiw 0
%endif
%define CLASS_c_jr 0
%define CLASS_c_jalr 0
%define CLASS_ldw 2
%define CLASS_ldwu 2
%define CLASS_ldd 2
%define CLASS_ldh 2
%define CLASS_ldhu 2
%define CLASS_ldb 2
%define CLASS_ldbu 2
%define CLASS_stw 2
%define CLASS_std 2
%define CLASS_sth 2
%define CLASS_stb 2
%define CLASS_flw 2
%define CLASS_fld 2
%define CLASS_fsw 2
%define CLASS_fsd 2
%define CLASS_c_ldw 2
%define CLASS_c_stw 2
%define CLASS_c_ldd 2
%define CLASS_c_std 2
%define CLASS_c_fld 2
%define CLASS_c_fsd 2
%define CLASS_c_flw 2
%define CLASS_c_fsw 2
%define CLASS_c_step 2
%assign COPY  0
%assign HID   0
%assign COLOR 0
%define TABLE_OFFSET (COLOR * 256 * 8)

%define REG(x)  XW [RREGS + x * XB]
;; The low 32 bits of a register (RV64 W-instructions)
%define REG32(x) dword [RREGS + x * XB]
%define FREG(x) [RREGS + FPOFF + x * 8]

;; Every handler is entered with rdx = the whole 8-byte decoder entry:
;;   dl = bytecode, bits 32..63 = instruction operands.
%macro DISPATCH 0
	mov rdx, [rbx]
	movzx eax, dl
	jmp [r14 + rax * 8 + TABLE_OFFSET]
%endmacro

;; Advance the decoder pointer by %1 bytes and dispatch
%macro NEXT 1
	mov rdx, [rbx + %1]
	add rbx, %1
	movzx eax, dl
	jmp [r14 + rax * 8 + TABLE_OFFSET]
%endmacro

;; Advance the decoder pointer by %1 bytes and dispatch, but when the next
;; instruction has the same bytecode, branch directly to the start of this
;; handler. A run of identical instructions (ld/sd in prologues, epilogues and
;; copy loops) is then a conditional branch, predicted with the global history,
;; instead of an indirect jump that targets itself and then something else.
%macro NEXT_SELF 1
	mov rdx, [rbx + %1]
	add rbx, %1
	movzx eax, dl
	cmp dl, dil             ; rdi = this entry (see READ_ADDRESS)
	je CUR_HANDLER
	jmp [r14 + rax * 8 + TABLE_OFFSET]
%endmacro

%macro NEXT_INSTR 0
	NEXT I4
%endmacro
%macro NEXT_MEM_INSTR 0
%ifndef NO_SELF_MEM
	NEXT_SELF I4
%else
	NEXT I4
%endif
%endmacro
%macro NEXT_MEM_C_INSTR 0
%ifndef NO_SELF_MEM
	NEXT_SELF I2
%else
	NEXT I2
%endif
%endmacro
%macro NEXT_C_INSTR 0
	NEXT I2
%endmacro

;; Dispatch the decoder entry at rbx, which begins a block (the target of a
;; branch or jump, or the instruction after one). COUNTING: count the block.
%macro DISPATCH_BLOCK 0
%if COUNTING && COMPRESSED
	mov rdx, [rbx]
	mov eax, edx
	shr eax, 24             ; icount: the instructions from here to the block end
	add r12, rax
	movzx eax, dl
	jmp [r14 + rax * 8 + TABLE_OFFSET]
%elif COUNTING
	mov rdx, [rbx]
	mov eax, edx
	shr eax, 16             ; idxend: the instructions after this one in its block
	lea r12, [r12 + rax + 1]
	movzx eax, dl
	jmp [r14 + rax * 8 + TABLE_OFFSET]
%else
	DISPATCH
%endif
%endmacro
;; Advance by %1 bytes to the next block (no overflow check)
%macro NEXT_BLOCK 1
%if COUNTING
	add rbx, %1
	DISPATCH_BLOCK
%else
	NEXT %1
%endif
%endmacro
%macro NEXT_BLOCK_INSTR 0
	NEXT_BLOCK I4
%endmacro
%macro NEXT_BLOCK_C_INSTR 0
	NEXT_BLOCK I2
%endmacro
;; COUNTING: leave the dispatch when the counter has reached the max, at the
;; pc of the decoder entry in rbx (_RBX) or at the guest address in xax (_EAX).
;; Inside a handler the jump goes to a stub after its end, like FALLBACK_IF,
;; so that it can be a short one.
%macro CHECK_OVERFLOW_RBX 0
%if COUNTING
	cmp r12, r11
%ifdef CUR_HANDLER
	jae CUR_HANDLER %+ _overflow_rbx
	%define NEED_OVERFLOW_RBX
%else
	jae exit_overflow_rbx
%endif
%endif
%endmacro
%macro CHECK_OVERFLOW_EAX 0
%if COUNTING
	cmp r12, r11
%ifdef CUR_HANDLER
	jae CUR_HANDLER %+ _overflow
	%define NEED_OVERFLOW_EAX
%else
	jae exit_overflow
%endif
%endif
%endmacro

;; %1 = 64-bit register receiving the pc of the current instruction
%macro RECONSTRUCT_PC 1
	mov %1, rbx
	sub %1, RDEC
	shr %1, PC2DEC
%endmacro

;; Branch by the signed 16-bit byte offset in the instruction.
;; %1 = 1: the branch may go backwards, so check the counter first
%macro TAKE_BRANCH 0-1 0
	movsx rax, word [rbx + 4]
	lea rbx, [rbx + rax * BRSCALE]
%if %1
	CHECK_OVERFLOW_RBX
%endif
	DISPATCH_BLOCK
%endmacro

;; Defines copy COPY of a handler. The exported symbol <prefix><name> is an
;; array with the address of every copy.
%macro HANDLER 1
	%assign HID HID+1
%ifdef CLASS_COLORS
	%ifdef CLASS_%1
	%assign %%class CLASS_%1
	%else
	%assign %%class CLASS_DEFAULT
	%endif
	;; The color is the class of this handler, so the copy that runs the next
	;; guest instruction is selected by what kind of instruction preceded it.
	%assign COLOR (%%class % NCOPIES)
%else
	%assign COLOR ((HID + COPY * COLOR_MUL) % NCOPIES)
%endif
	FLUSH_FALLBACK
	%if COPY == 0
	[section RELRO_SECTION]
	align 8
	global %[PFX]%1
	%[PFX]%1:
	%assign %%i 0
	%rep NCOPIES
		dq %[PFX]%1_%[%%i]
		%assign %%i %%i+1
	%endrep
	__?SECT?__
	%endif
	align HALIGN
	%[PFX]%1_%[COPY]:
	%xdefine CUR_HANDLER %[PFX]%1_%[COPY]
	%xdefine CUR_FALLBACK %[PFX]%1_%[COPY]_fallback
	%define FALLBACK_TARGET do_fallback
%endmacro
;; A handler for a 2-byte instruction: its fallback continues 2 bytes later
%macro C_HANDLER 1
	HANDLER %1
	%define FALLBACK_TARGET do_c_fallback
%endmacro

;; Go to the fallback (execute this instruction in C++) when the condition
;; holds. The jump goes to a stub after the end of the handler, so that it
;; can be a short one: handlers have to fit in 64 bytes.
%macro FALLBACK_IF 1   ; %1 = jcc
	%1 CUR_FALLBACK
	%define NEED_FALLBACK
%endmacro
%macro FLUSH_FALLBACK 0
%ifdef NEED_FALLBACK
CUR_FALLBACK:
	jmp FALLBACK_TARGET
	%undef NEED_FALLBACK
%endif
%ifdef NEED_OVERFLOW_RBX
CUR_HANDLER %+ _overflow_rbx:
	jmp exit_overflow_rbx
	%undef NEED_OVERFLOW_RBX
%endif
%ifdef NEED_OVERFLOW_EAX
CUR_HANDLER %+ _overflow:
	jmp exit_overflow
	%undef NEED_OVERFLOW_EAX
%endif
%endmacro

section RODATA_SECTION
global %[PFX]ncopies
%[PFX]ncopies: dd NCOPIES

%if WIN64
;; HALIGN needs the section to be at least as aligned as the handlers
section .text code align=64
%else
section .text
%endif

;; uint32_t <prefix>dispatch(AsmDispatchContext* ctx)
;; Starts executing at ctx->pc, which must be inside the execute segment.
;; Returns an EXIT_* code with ctx->pc set accordingly.
align 16
global %[PFX]dispatch
%[PFX]dispatch:
%if WIN64
	;; The unwind codes at the end of this file describe this prologue
	push rbx
.uw_rbx:
	push rbp
.uw_rbp:
	push rdi
.uw_rdi:
	push rsi
.uw_rsi:
	push r12
.uw_r12:
	push r13
.uw_r13:
	push r14
.uw_r14:
	push r15
.uw_r15:
	sub rsp, FRAME_SIZE
.uw_alloc:
	mov CTX_SLOT, rcx
	mov rdi, rcx
%else
	push rbx
	push rbp
	push r12
	push r13
	push r14
	push r15
	push rdi                ; [rsp] = context
%endif
	mov RREGS,  [rdi + CTX_REGS]
	mov RDEC,   [rdi + CTX_DECODER]
	mov RARENA, [rdi + CTX_ARENA]
	mov r14,    [rdi + CTX_TABLE]
	mov xr8,  XW [rdi + CTX_RBOUND]
	mov xr9,  XW [rdi + CTX_RODATA]
	mov xr10, XW [rdi + CTX_WBOUND]
%if COUNTING
	mov r11, [rdi + CTX_MAX]
	mov r12, [rdi + CTX_COUNTER]
%else
	mov xr11, XW [rdi + CTX_BEGIN]
	mov xr12, XW [rdi + CTX_LENGTH]
%endif
	mov xax,  XW [rdi + CTX_PC]
	lea RBTAB, [backward_table_end]
	lea rbx, [RDEC + rax * BRSCALE]
%if COUNTING
	cmp byte [rdi + CTX_ENTER_COUNT], 0
	je .uncounted
	DISPATCH_BLOCK
.uncounted:
%endif
	DISPATCH

;; Leave the dispatch: eax = exit code, ctx->pc already written
exit_dispatch:
%if COUNTING
	mov rdi, CTX_SLOT
	mov [rdi + CTX_COUNTER], r12
	mov [rdi + CTX_MAX], r11
%endif
%if WIN64
	;; The canonical epilogue form, which the Windows unwinder recognizes
	add rsp, FRAME_SIZE
	pop r15
	pop r14
	pop r13
	pop r12
	pop rsi
	pop rdi
	pop rbp
	pop rbx
%else
	pop rdi
	pop r15
	pop r14
	pop r13
	pop r12
	pop rbp
	pop rbx
%endif
	ret

%if COUNTING
;; The counter reached the max: stop at the pc of rbx, or at xax
exit_overflow_rbx:
	RECONSTRUCT_PC rax
exit_overflow:
	mov rdi, CTX_SLOT
	mov XW [rdi + CTX_PC], xax
	mov eax, EXIT_OVERFLOW
	jmp exit_dispatch
%endif

;; Jump to xax, which is outside the current execute segment
exit_jump:
	mov rdi, CTX_SLOT
	mov XW [rdi + CTX_PC], xax
	mov eax, EXIT_JUMP
	jmp exit_dispatch

do_stop:
	RECONSTRUCT_PC rax
	add xax, 4
	mov rdi, CTX_SLOT
	mov XW [rdi + CTX_PC], xax
	mov eax, EXIT_STOP
	jmp exit_dispatch

;; Jump to the address in xax: within the segment it is just a new
;; decoder pointer, otherwise C++ has to find the segment.
%macro JUMP_TO_EAX 0
%if COUNTING
	CHECK_OVERFLOW_EAX
	mov rdi, CTX_SLOT
	mov xcx, xax
	sub xcx, XW [rdi + CTX_BEGIN]
	cmp xcx, XW [rdi + CTX_LENGTH]
%else
	mov xcx, xax
	sub xcx, xr11
	cmp xcx, xr12
%endif
	jae exit_jump
	lea rbx, [RDEC + rax * BRSCALE]
	DISPATCH_BLOCK
%endmacro

;; <prefix>call asked to continue at ctx->pc
call_goto:
	mov rdi, CTX_SLOT
	mov xax, XW [rdi + CTX_PC]
	JUMP_TO_EAX

;; Execute the current decoder entry in C++, see <prefix>call.
;; The pinned caller-saved registers are kept on the stack, which is 16-byte
;; aligned in every handler. %1 = KIND_*, %2 = NEXT macro
%macro CALL_CPP 2
%if WIN64
	;; rsi (RREGS) is callee-saved, and the frame already has the shadow space
	mov R11_SLOT, r11
	mov rcx, CTX_SLOT       ; context
%if COUNTING
	mov [rcx + CTX_COUNTER], r12
	mov [rcx + CTX_MAX], r11
%endif
	mov rdx, rbx            ; decoder entry
	mov r8d, %1
	call %[PFX]call
	mov r11, R11_SLOT
%else
	push r11
	push RREGS
	mov rdi, [rsp + 16]     ; context
%if COUNTING
	mov [rdi + CTX_COUNTER], r12
	mov [rdi + CTX_MAX], r11
%endif
	mov rsi, rbx            ; decoder entry
	mov edx, %1
	call %[PFX]call wrt ..plt
	pop RREGS
	pop r11
%endif
	;; The arena boundaries can change in any system call
	mov rdi, CTX_SLOT
	mov xr8,  XW [rdi + CTX_RBOUND]
	mov xr9,  XW [rdi + CTX_RODATA]
	mov xr10, XW [rdi + CTX_WBOUND]
%if COUNTING
	;; So can the counters (penalties, stopping)
	mov r12, [rdi + CTX_COUNTER]
	mov r11, [rdi + CTX_MAX]
%endif
	cmp eax, CALL_NEXT
	jne %%special
	%2
%%special:
	ja call_goto
	jmp exit_dispatch       ; eax = exit code, ctx->pc written by C++
%endmacro

;; A fast path could not handle the current instruction (a load or store
;; outside the flat arena, a misaligned jump, ...): C++ executes it from
;; memory (CPU::step_one), raising whatever exception is due, without leaving
;; the dispatch.
do_fallback:
	CALL_CPP KIND_STEP, NEXT_INSTR
%if COMPRESSED
do_c_fallback:
	CALL_CPP KIND_C_STEP, NEXT_C_INSTR
%endif

;; Division, RISC-V semantics (no traps)
%macro DIVISION 1   ; %1 = divu, remu, div, rem
	movzx eax, byte [rbx + 7]
	movzx ecx, byte [rbx + 6]
	movzx edi, word [rbx + 4]
	mov xax, REG(rax)
	mov xcx, REG(rcx)
	test xcx, xcx
	jz %%by_zero
%ifidn %1, div
	cmp xcx, -1
	je %%minus_one
%if XLEN == 64
	cqo
%else
	cdq
%endif
	idiv xcx
%elifidn %1, rem
	cmp xcx, -1
	je %%minus_one
%if XLEN == 64
	cqo
%else
	cdq
%endif
	idiv xcx
	mov xax, xdx
%elifidn %1, divu
	xor edx, edx
	div xcx
%else
	xor edx, edx
	div xcx
	mov xax, xdx
%endif
	mov REG(rdi), xax
	NEXT_INSTR
%%by_zero:
%ifidn %1, divu
	mov xax, -1
%elifidn %1, div
	mov xax, -1
%endif
	mov REG(rdi), xax       ; rem/remu by zero: the dividend
	NEXT_INSTR
;; Division by -1 is negation, which also gives MIN / -1 = MIN without the
;; overflow trap, and the remainder is always 0
%ifidn %1, div
%%minus_one:
	neg xax
	mov REG(rdi), xax
	NEXT_INSTR
%elifidn %1, rem
%%minus_one:
	mov REG(rdi), 0
	NEXT_INSTR
%endif
%endmacro

%macro ITYPE_LOAD_SRC 0   ; xax = REG(rs2), edx = rd
	movzx ecx, byte [rbx + 6]
	movzx edx, byte [rbx + 7]
	mov xax, REG(rcx)
%endmacro
%macro OP_LOAD 0   ; xax = REG(rs1), xcx = REG(rs2), edx = rd
	movzx eax, byte [rbx + 7]
	movzx ecx, byte [rbx + 6]
	movzx edx, word [rbx + 4]
	mov xax, REG(rax)
	mov xcx, REG(rcx)
%endmacro
%macro OP_LOAD32 0   ; eax = low half of REG(rs1), ecx = of REG(rs2), edx = rd
	movzx eax, byte [rbx + 7]
	movzx ecx, byte [rbx + 6]
	movzx edx, word [rbx + 4]
	mov eax, REG32(rax)
	mov ecx, REG32(rcx)
%endmacro
;; Dispatch the instruction after a branch that was not taken (%1 = NEXT macro)
;; with the color NT_COLOR rather than the color of control flow, which the
;; taken path (a branch target) keeps. Otherwise the fall-through instruction
;; and the branch target run the same copy whenever they have the same
;; bytecode, and that copy's dispatch jump alternates between their
;; successors. In CoreMark's core_list_find loop:
;;   lw a0,0(a0); beqz a0,out; lw a5,4(a0); lh a5,2(a5); bne a5,a4,loop
;; both LWs ran ldw copy 0 (one after the taken BNE, one after the not-taken
;; BEQZ), whose jump alternated between BEQZ and LH and mispredicted almost
;; every time. Separating them cut CoreMark's indirect mispredictions from 430M
;; to ~105M and its cycles by 6-9% (rv32gb), with Dhrystone, fib, STREAM and
;; rvctest unchanged. Color 2 measured better than 1. A 4th copy just for this
;; (or a 5th for jumps) was no faster on CoreMark and cost Dhrystone 2-4%.
%ifndef NT_COLOR
%define NT_COLOR 2
%endif
%macro NOT_TAKEN 1
	%assign %%saved COLOR
	%assign COLOR NT_COLOR
	%1
	%assign COLOR %%saved
%endmacro
;; %1 = jcc taken when the branch is taken, %2 = 1 when it may go backwards
%macro BRANCH 2
	movzx eax, byte [rbx + 7]
	movzx ecx, byte [rbx + 6]
	mov xax, REG(rax)
	cmp xax, REG(rcx)
	%1 %%taken
	NOT_TAKEN NEXT_BLOCK_INSTR
%%taken:
	TAKE_BRANCH %2
%endmacro
%macro BRANCH_BACKWARD 1   ; %1 = jcc taken when the branch is NOT taken
	movzx eax, byte [rbx + 7]
	movzx ecx, byte [rbx + 6]
	mov xax, REG(rax)
	cmp xax, REG(rcx)
	%1 %%not_taken
%ifdef NO_BSTUBS
	TAKE_BRANCH 1
%else
	movsx rax, word [rbx + 4]   ; <= 0 for BEQ/BNE (the rewriter makes the rest _FW)
	jmp [RBTAB + rax * (8 / STUB_GRAN)]
%endif
%%not_taken:
	NOT_TAKEN NEXT_BLOCK_INSTR
%endmacro

;; Memory handlers load the operands from the entry in memory, like the
;; others: those loads can issue together with the load of the entry itself,
;; whereas taking them out of rdx adds ALU latency in front of the address,
;; which latency-bound code (CoreMark: -5%) pays for. The entry is copied to
;; rdi, so that the dispatch can compare the next bytecode with this one
;; without another load (see NEXT_SELF).
;; FasterItype: imm16 @4, rs2 @6, rs1 @7

;; xax = guest address REG(rs @%1) + imm16, or go to the fallback when it is
;; not readable in the flat arena
%macro READ_ADDRESS 1
	mov rdi, rdx
	movzx ecx, byte [rbx + %1]
	movsx xax, word [rbx + 4]
	add xax, REG(rcx)
	lea xcx, [rax - RWREAD_BEGIN]
	cmp xcx, xr8
	FALLBACK_IF jae
%endmacro
;; Same for writes: xax = guest address REG(rs @%1) + imm16
%macro WRITE_ADDRESS 1
	mov rdi, rdx
	movzx ecx, byte [rbx + %1]
	movsx xax, word [rbx + 4]
	add xax, REG(rcx)
	mov xdx, xax
	sub xdx, xr9
	cmp xdx, xr10
	FALLBACK_IF jae
%endmacro

;; FasterItype: imm16 @4, rs2 (base) @6, rs1 (dst) @7
%macro LOAD 2   ; %1 = load instruction with xax as destination, %2 = NEXT
	READ_ADDRESS 6
	movzx edx, byte [rbx + 7]
	%1
	mov REG(rdx), xax
	%2
%endmacro
;; FasterItype: imm16 @4, rs2 (value) @6, rs1 (base) @7
%macro STORE 2   ; %1 = store instruction, value in xcx, %2 = NEXT
	WRITE_ADDRESS 7
	movzx ecx, byte [rbx + 6]
	mov xcx, REG(rcx)
	%1
	%2
%endmacro

;; FasterFloatType: func @4 (0 = single, 1 = double), rd @5, rs2 @6, rs1 @7
;; Without NaN-boxing a single-precision result only writes the low half,
;; like fp64reg::set_float().
%macro FP_ARITH 1   ; %1 = add, sub, mul, div
	movzx eax, byte [rbx + 7]
	movzx ecx, byte [rbx + 6]
	movzx edi, byte [rbx + 5]
	bt rdx, 32
	jc %%double
	vmovss xmm0, FREG(rax)
	v%1ss xmm0, xmm0, FREG(rcx)
	vmovss FREG(rdi), xmm0
	NEXT_INSTR
	align 64                ; each path fits in its own 64-byte window
%%double:
	vmovsd xmm0, FREG(rax)
	v%1sd xmm0, xmm0, FREG(rcx)
	vmovsd FREG(rdi), xmm0
	NEXT_INSTR
%endmacro


%macro ALL_HANDLERS 0
	%assign HID 0
;; RV32I_BC_INVALID and every unknown bytecode
HANDLER fallback
	CALL_CPP KIND_INVALID, NEXT_INSTR
HANDLER stop
	jmp do_stop

;; -== Immediate ALU, FasterItype: imm16 @4, rs2 (src) @6, rs1 (dst) @7 ==-

HANDLER addi
	rorx rax, rdx, 32       ; ax = imm, bits 16..23 = rs2, 24..31 = rs1
	movsx xdi, ax
	shr eax, 16
	movzx ecx, al
	movzx eax, ah
	add xdi, REG(rcx)
	mov REG(rax), xdi
	NEXT_INSTR

;; FasterImmediate: rd @4, imm16 @6
HANDLER li
	movzx ecx, byte [rbx + 4]
	movsx xax, word [rbx + 6]
	mov REG(rcx), xax
	NEXT_INSTR

;; FasterMove: rs1 @4, rd @5
HANDLER mv
	movzx ecx, byte [rbx + 4]
	movzx eax, byte [rbx + 5]
	mov xcx, REG(rcx)
	mov REG(rax), xcx
	NEXT_INSTR

HANDLER slli
	ITYPE_LOAD_SRC
	movzx ecx, word [rbx + 4]
	shl xax, cl
	mov REG(rdx), xax
	NEXT_INSTR

HANDLER srli
	ITYPE_LOAD_SRC
	movzx ecx, word [rbx + 4]
	shr xax, cl
	mov REG(rdx), xax
	NEXT_INSTR

HANDLER srai
	ITYPE_LOAD_SRC
	movzx ecx, word [rbx + 4]
	sar xax, cl
	mov REG(rdx), xax
	NEXT_INSTR

HANDLER slti
	ITYPE_LOAD_SRC
	movsx xcx, word [rbx + 4]
	xor edi, edi
	cmp xax, xcx
	setl dil
	mov REG(rdx), xdi
	NEXT_INSTR

HANDLER sltiu
	ITYPE_LOAD_SRC
	movsx xcx, word [rbx + 4]
	xor edi, edi
	cmp xax, xcx
	setb dil
	mov REG(rdx), xdi
	NEXT_INSTR

HANDLER xori
	ITYPE_LOAD_SRC
	movsx xcx, word [rbx + 4]
	xor xax, xcx
	mov REG(rdx), xax
	NEXT_INSTR

HANDLER ori
	ITYPE_LOAD_SRC
	movsx xcx, word [rbx + 4]
	or xax, xcx
	mov REG(rdx), xax
	NEXT_INSTR

HANDLER andi
	ITYPE_LOAD_SRC
	movsx xcx, word [rbx + 4]
	and xax, xcx
	mov REG(rdx), xax
	NEXT_INSTR

HANDLER sext_b
	ITYPE_LOAD_SRC
	movsx xax, al
	mov REG(rdx), xax
	NEXT_INSTR

HANDLER sext_h
	ITYPE_LOAD_SRC
	movsx xax, ax
	mov REG(rdx), xax
	NEXT_INSTR

HANDLER bseti
	ITYPE_LOAD_SRC
	movzx ecx, word [rbx + 4]
	bts xax, xcx
	mov REG(rdx), xax
	NEXT_INSTR

HANDLER bexti
	ITYPE_LOAD_SRC
	movzx ecx, word [rbx + 4]
	shr xax, cl
	and eax, 1
	mov REG(rdx), xax
	NEXT_INSTR

;; -== Upper immediates, FasterJtype: offset24 @4..6, rd @7 ==-

HANDLER lui
	mov eax, dword [rbx + 4]
	movzx edx, byte [rbx + 7]
	shl eax, 8
%if XLEN == 64
	movsxd rax, eax
%endif
	mov REG(rdx), xax
	NEXT_INSTR

HANDLER auipc
	mov eax, dword [rbx + 4]
	movzx edx, byte [rbx + 7]
	shl eax, 8
%if XLEN == 64
	movsxd rax, eax
%endif
	RECONSTRUCT_PC rcx
	add xax, xcx
	mov REG(rdx), xax
	NEXT_INSTR

;; -== Register ALU, FasterOpType: rd16 @4, rs2 @6, rs1 @7 ==-

HANDLER op_add
	rorx rax, rdx, 48       ; al = rs2, ah = rs1
	shr rdx, 32             ; dx = rd
	movzx ecx, al
	movzx eax, ah
	movzx edx, dx
	mov xax, REG(rax)
	add xax, REG(rcx)
	mov REG(rdx), xax
	NEXT_INSTR

HANDLER op_sub
	OP_LOAD
	sub xax, xcx
	mov REG(rdx), xax
	NEXT_INSTR

HANDLER op_sll
	OP_LOAD
	shl xax, cl
	mov REG(rdx), xax
	NEXT_INSTR

HANDLER op_srl
	OP_LOAD
	shr xax, cl
	mov REG(rdx), xax
	NEXT_INSTR

HANDLER op_sra
	OP_LOAD
	sar xax, cl
	mov REG(rdx), xax
	NEXT_INSTR

HANDLER op_slt
	OP_LOAD
	xor edi, edi
	cmp xax, xcx
	setl dil
	mov REG(rdx), xdi
	NEXT_INSTR

HANDLER op_sltu
	OP_LOAD
	xor edi, edi
	cmp xax, xcx
	setb dil
	mov REG(rdx), xdi
	NEXT_INSTR

HANDLER op_xor
	OP_LOAD
	xor xax, xcx
	mov REG(rdx), xax
	NEXT_INSTR

HANDLER op_or
	OP_LOAD
	or xax, xcx
	mov REG(rdx), xax
	NEXT_INSTR

HANDLER op_and
	OP_LOAD
	and xax, xcx
	mov REG(rdx), xax
	NEXT_INSTR

HANDLER op_mul
	OP_LOAD
	imul xax, xcx
	mov REG(rdx), xax
	NEXT_INSTR

HANDLER op_div
	DIVISION div
HANDLER op_divu
	DIVISION divu
HANDLER op_rem
	DIVISION rem
HANDLER op_remu
	DIVISION remu

HANDLER op_zext_h
	OP_LOAD
	movzx eax, ax
	mov REG(rdx), xax
	NEXT_INSTR

HANDLER op_sh1add
	OP_LOAD
	lea xax, [rcx + rax * 2]
	mov REG(rdx), xax
	NEXT_INSTR

HANDLER op_sh2add
	OP_LOAD
	lea xax, [rcx + rax * 4]
	mov REG(rdx), xax
	NEXT_INSTR

HANDLER op_sh3add
	OP_LOAD
	lea xax, [rcx + rax * 8]
	mov REG(rdx), xax
	NEXT_INSTR

%if XLEN == 64
;; -== RV64 32-bit operations, sign-extended to 64 bits ==-

HANDLER addiw
	movzx ecx, byte [rbx + 6]
	movsx edi, word [rbx + 4]
	movzx edx, byte [rbx + 7]
	add edi, REG32(rcx)
	movsxd rdi, edi
	mov REG(rdx), rdi
	NEXT_INSTR

HANDLER slliw
	movzx eax, byte [rbx + 6]
	movzx ecx, word [rbx + 4]
	movzx edx, byte [rbx + 7]
	mov eax, REG32(rax)
	shl eax, cl
	movsxd rax, eax
	mov REG(rdx), rax
	NEXT_INSTR

HANDLER srliw
	movzx eax, byte [rbx + 6]
	movzx ecx, word [rbx + 4]
	movzx edx, byte [rbx + 7]
	mov eax, REG32(rax)
	shr eax, cl
	movsxd rax, eax
	mov REG(rdx), rax
	NEXT_INSTR

HANDLER sraiw
	movzx eax, byte [rbx + 6]
	movzx ecx, word [rbx + 4]
	movzx edx, byte [rbx + 7]
	mov eax, REG32(rax)
	sar eax, cl
	movsxd rax, eax
	mov REG(rdx), rax
	NEXT_INSTR

HANDLER op_addw
	OP_LOAD32
	add eax, ecx
	movsxd rax, eax
	mov REG(rdx), rax
	NEXT_INSTR

HANDLER op_subw
	OP_LOAD32
	sub eax, ecx
	movsxd rax, eax
	mov REG(rdx), rax
	NEXT_INSTR

HANDLER op_mulw
	OP_LOAD32
	imul eax, ecx
	movsxd rax, eax
	mov REG(rdx), rax
	NEXT_INSTR

;; rd = zext32(rs1) + rs2
HANDLER op_add_uw
	movzx eax, byte [rbx + 7]
	movzx ecx, byte [rbx + 6]
	movzx edx, word [rbx + 4]
	mov eax, REG32(rax)
	add rax, REG(rcx)
	mov REG(rdx), rax
	NEXT_INSTR

HANDLER op_sh1add_uw
	movzx eax, byte [rbx + 7]
	movzx ecx, byte [rbx + 6]
	movzx edx, word [rbx + 4]
	mov eax, REG32(rax)
	mov rcx, REG(rcx)
	lea rax, [rcx + rax * 2]
	mov REG(rdx), rax
	NEXT_INSTR

HANDLER op_sh2add_uw
	movzx eax, byte [rbx + 7]
	movzx ecx, byte [rbx + 6]
	movzx edx, word [rbx + 4]
	mov eax, REG32(rax)
	mov rcx, REG(rcx)
	lea rax, [rcx + rax * 4]
	mov REG(rdx), rax
	NEXT_INSTR
%endif ; XLEN == 64

;; -== Calls into C++ ==-

HANDLER function
	CALL_CPP KIND_FUNCTION, NEXT_INSTR
HANDLER funcblock
	CALL_CPP KIND_FUNCBLOCK, NEXT_INSTR   ; C++ always continues at ctx->pc
HANDLER syscall
	CALL_CPP KIND_SYSCALL, NEXT_BLOCK_INSTR
HANDLER dyncall
	CALL_CPP KIND_DYNCALL, NEXT_INSTR
HANDLER system
	CALL_CPP KIND_SYSTEM, NEXT_BLOCK_INSTR
;; Native code (binary translation, asmjit): C++ runs it for as long as it
;; can, then continues at ctx->pc
HANDLER translator
	CALL_CPP KIND_TRANSLATOR, NEXT_INSTR
HANDLER asmjit
	CALL_CPP KIND_ASMJIT, NEXT_INSTR
;; Native code was activated in the background (livepatch.hpp): C++ switches
;; to the patched decoder cache and re-enters at this instruction
HANDLER livepatch
	CALL_CPP KIND_LIVEPATCH, NEXT_INSTR
;; A 4-byte instruction that C++ executes from memory (CPU::step_one), for
;; bytecodes whose fast path here would be incorrect, such as FP arithmetic
;; with FCSR emulation. Cheaper than the fallback, which leaves the dispatch.
HANDLER step
	CALL_CPP KIND_STEP, NEXT_INSTR

;; -== Branches, FasterItype: imm16 @4, rs2 @6, rs1 @7 ==-

;; Backward branches are mostly taken (loops): the taken path falls through

HANDLER beq
	BRANCH_BACKWARD jne
HANDLER bne
	BRANCH_BACKWARD je
HANDLER beq_fw
	BRANCH je, 0
HANDLER bne_fw
	BRANCH jne, 0
HANDLER blt
	BRANCH jl, 1
HANDLER bge
	BRANCH jge, 1
HANDLER bltu
	BRANCH jb, 1
HANDLER bgeu
	BRANCH jae, 1

;; -== Jumps ==-

;; FasterJtype: rd = pc + 4, pc += sext(offset24)
HANDLER jal
	RECONSTRUCT_PC rax
	movzx edx, byte [rbx + 7]
	add xax, 4
	mov REG(rdx), xax
	mov eax, dword [rbx + 4]
	shl eax, 8
	sar eax, 8
	movsxd rax, eax
	lea rbx, [rbx + rax * BRSCALE]
	CHECK_OVERFLOW_RBX
	DISPATCH_BLOCK

HANDLER fast_jal
	movsxd rax, dword [rbx + 4]
	lea rbx, [rbx + rax * BRSCALE]
	CHECK_OVERFLOW_RBX
	DISPATCH_BLOCK

HANDLER fast_call
	RECONSTRUCT_PC rax
	add xax, 4
	mov REG(1), xax
	movsxd rax, dword [rbx + 4]
	lea rbx, [rbx + rax * BRSCALE]
	CHECK_OVERFLOW_RBX
	DISPATCH_BLOCK

;; FasterItype: target = REG(rs2) + imm, rs1 = link register (0 = none)
HANDLER jalr
	movzx ecx, byte [rbx + 6]
	movsx xax, word [rbx + 4]
	add xax, REG(rcx)
	and xax, -2
%if COMPRESSED == 0
	test eax, 3
	FALLBACK_IF jnz   ; C++ raises the misaligned exception
%endif
	movzx edx, byte [rbx + 7]
	test edx, edx
	jz .no_link
	RECONSTRUCT_PC rcx
	add xcx, 4
	mov REG(rdx), xcx
.no_link:
	JUMP_TO_EAX

;; -== Loads ==-

HANDLER ldw
%if XLEN == 64
	LOAD {movsxd rax, dword [RARENA + rax]}, NEXT_MEM_INSTR
%else
	LOAD {mov eax, dword [RARENA + rax]}, NEXT_MEM_INSTR
%endif
HANDLER ldh
	LOAD {movsx xax, word [RARENA + rax]}, NEXT_MEM_INSTR
HANDLER ldhu
	LOAD {movzx eax, word [RARENA + rax]}, NEXT_MEM_INSTR
HANDLER ldb
	LOAD {movsx xax, byte [RARENA + rax]}, NEXT_MEM_INSTR
HANDLER ldbu
	LOAD {movzx eax, byte [RARENA + rax]}, NEXT_MEM_INSTR
%if XLEN == 64
HANDLER ldwu
	LOAD {mov eax, dword [RARENA + rax]}, NEXT_MEM_INSTR
HANDLER ldd
	LOAD {mov rax, qword [RARENA + rax]}, NEXT_MEM_INSTR
%endif

;; -== Stores ==-

HANDLER stw
	STORE {mov dword [RARENA + rax], ecx}, NEXT_MEM_INSTR
HANDLER sth
	STORE {mov word [RARENA + rax], cx}, NEXT_MEM_INSTR
HANDLER stb
	STORE {mov byte [RARENA + rax], cl}, NEXT_MEM_INSTR
%if XLEN == 64
HANDLER std
	STORE {mov qword [RARENA + rax], rcx}, NEXT_MEM_INSTR
%endif

;; -== Floating point loads and stores, FasterItype like the integer ones ==-

HANDLER flw
	READ_ADDRESS 6
	movzx edx, byte [rbx + 7]
	mov eax, dword [RARENA + rax]
	mov dword FREG(rdx), eax    ; no NaN-boxing (C++ maps FLW only without it)
	NEXT_MEM_INSTR
HANDLER fld
	READ_ADDRESS 6
	movzx edx, byte [rbx + 7]
	mov rax, qword [RARENA + rax]
	mov qword FREG(rdx), rax
	NEXT_MEM_INSTR
HANDLER fsw
	WRITE_ADDRESS 7
	movzx ecx, byte [rbx + 6]
	mov ecx, dword FREG(rcx)
	mov dword [RARENA + rax], ecx
	NEXT_MEM_INSTR
HANDLER fsd
	WRITE_ADDRESS 7
	movzx ecx, byte [rbx + 6]
	mov rcx, qword FREG(rcx)
	mov qword [RARENA + rax], rcx
	NEXT_MEM_INSTR

;; -== Floating point arithmetic (C++ maps these only without FCSR emulation
;;     and NaN-boxing, which need flags and canonical NaNs) ==-

HANDLER fadd
	FP_ARITH add
HANDLER fsub
	FP_ARITH sub
HANDLER fmul
	FP_ARITH mul
HANDLER fdiv
	FP_ARITH div

;; The original instruction (R4type): rd @7, rs1 @15, rs2 @20, funct2 @25,
;; rs3 @27. rd = rs1 * rs2 + rs3, rounded once.
HANDLER fmadd
	mov eax, dword [rbx + 4]
	test eax, 1 << 26           ; funct2 >= 2 (half, quad)
	FALLBACK_IF jnz
	mov ecx, eax
	shr ecx, 15
	and ecx, 31
	mov edi, eax
	shr edi, 20
	and edi, 31
	bt eax, 25
	jc .double
	vmovss xmm0, FREG(rcx)
	vmovss xmm1, FREG(rdi)
	mov ecx, eax
	shr ecx, 27
	shr eax, 7
	and eax, 31
	vfmadd213ss xmm0, xmm1, FREG(rcx)
	vmovss FREG(rax), xmm0
	NEXT_INSTR
	align 64
.double:
	vmovsd xmm0, FREG(rcx)
	vmovsd xmm1, FREG(rdi)
	mov ecx, eax
	shr ecx, 27
	shr eax, 7
	and eax, 31
	vfmadd213sd xmm0, xmm1, FREG(rcx)
	vmovsd FREG(rax), xmm0
	NEXT_INSTR

%if COMPRESSED
;; -== Compressed bytecodes, one decoder entry each ==-

;; FasterItype: rs1 = dst @7, rs2 = src @6, imm16 @4
HANDLER c_addi
	movzx ecx, byte [rbx + 6]
	movsx xax, word [rbx + 4]
	movzx edx, byte [rbx + 7]
	add xax, REG(rcx)
	mov REG(rdx), xax
	NEXT_C_INSTR

HANDLER c_mv
	movzx ecx, byte [rbx + 4]
	movzx edx, byte [rbx + 5]
	mov xax, REG(rcx)
	mov REG(rdx), xax
	NEXT_C_INSTR

HANDLER c_slli
	movzx edx, byte [rbx + 7]
	movzx ecx, word [rbx + 4]
	shl REG(rdx), cl
	NEXT_C_INSTR

HANDLER c_srli
	movzx edx, byte [rbx + 7]
	movzx ecx, word [rbx + 4]
	shr REG(rdx), cl
	NEXT_C_INSTR

HANDLER c_andi
	movzx edx, byte [rbx + 7]
	movsx xax, word [rbx + 4]
	and REG(rdx), xax
	NEXT_C_INSTR

HANDLER c_srai
	movzx edx, byte [rbx + 7]
	movzx ecx, word [rbx + 4]
	sar REG(rdx), cl
	NEXT_C_INSTR

HANDLER c_sub
	movzx ecx, byte [rbx + 6]
	movzx edx, byte [rbx + 7]
	mov xax, REG(rcx)
	sub REG(rdx), xax
	NEXT_C_INSTR

HANDLER c_and
	movzx ecx, byte [rbx + 6]
	movzx edx, byte [rbx + 7]
	mov xax, REG(rcx)
	and REG(rdx), xax
	NEXT_C_INSTR

%if XLEN == 64
HANDLER c_subw
	movzx ecx, byte [rbx + 6]
	movzx edx, byte [rbx + 7]
	mov eax, REG32(rdx)
	sub eax, REG32(rcx)
	movsxd rax, eax
	mov REG(rdx), rax
	NEXT_C_INSTR

HANDLER c_addw
	movzx ecx, byte [rbx + 6]
	movzx edx, byte [rbx + 7]
	mov eax, REG32(rdx)
	add eax, REG32(rcx)
	movsxd rax, eax
	mov REG(rdx), rax
	NEXT_C_INSTR
%endif

HANDLER c_add
	movzx ecx, byte [rbx + 6]
	movzx edx, byte [rbx + 7]
	mov xax, REG(rcx)
	add REG(rdx), xax
	NEXT_C_INSTR

HANDLER c_xor
	movzx ecx, byte [rbx + 6]
	movzx edx, byte [rbx + 7]
	mov xax, REG(rcx)
	xor REG(rdx), xax
	NEXT_C_INSTR

HANDLER c_or
	movzx ecx, byte [rbx + 6]
	movzx edx, byte [rbx + 7]
	mov xax, REG(rcx)
	or REG(rdx), xax
	NEXT_C_INSTR

;; Taken compressed branches check the counter in either direction, like
;; PERFORM_BRANCH in cpu_dispatch.cpp
HANDLER c_bnez
	movzx eax, byte [rbx + 7]
	cmp REG(rax), 0
	jne .taken
	NOT_TAKEN NEXT_BLOCK_C_INSTR
.taken:
	TAKE_BRANCH 1

HANDLER c_beqz
	movzx eax, byte [rbx + 7]
	cmp REG(rax), 0
	je .taken
	NOT_TAKEN NEXT_BLOCK_C_INSTR
.taken:
	TAKE_BRANCH 1

HANDLER c_jmp
	TAKE_BRANCH 1

HANDLER c_jal_addiw
%if XLEN == 64
	;; C.ADDIW: rd = sext32(rd + imm)
	movzx edx, byte [rbx + 7]
	movsx eax, word [rbx + 4]
	add eax, REG32(rdx)
	movsxd rax, eax
	mov REG(rdx), rax
	NEXT_C_INSTR
%else
	;; C.JAL
	RECONSTRUCT_PC rax
	add eax, 2
	mov REG(1), eax
	TAKE_BRANCH 1
%endif

;; instr = register index
HANDLER c_jr
	mov ecx, dword [rbx + 4]
	mov xax, REG(rcx)
	and xax, -2
	JUMP_TO_EAX

HANDLER c_jalr
	mov ecx, dword [rbx + 4]
	mov xax, REG(rcx)
	and xax, -2
	RECONSTRUCT_PC rcx
	add xcx, 2
	mov REG(1), xcx
	JUMP_TO_EAX

;; FasterJtype: offset24 @4..6, rd @7
HANDLER c_lui
	movzx ecx, byte [rbx + 7]
	shr rdx, 32
	shl edx, 8
%if XLEN == 64
	movsxd rdx, edx
%endif
	mov REG(rcx), xdx
	NEXT_C_INSTR

C_HANDLER c_fld
	READ_ADDRESS 6
	movzx edx, byte [rbx + 7]
	mov rax, qword [RARENA + rax]
	mov qword FREG(rdx), rax
	NEXT_MEM_C_INSTR
C_HANDLER c_fsd
	WRITE_ADDRESS 7
	movzx ecx, byte [rbx + 6]
	mov rcx, qword FREG(rcx)
	mov qword [RARENA + rax], rcx
	NEXT_MEM_C_INSTR
%if XLEN == 32
C_HANDLER c_flw
	READ_ADDRESS 6
	movzx edx, byte [rbx + 7]
	mov eax, dword [RARENA + rax]
	mov dword FREG(rdx), eax
	NEXT_MEM_C_INSTR
C_HANDLER c_fsw
	WRITE_ADDRESS 7
	movzx ecx, byte [rbx + 6]
	mov ecx, dword FREG(rcx)
	mov dword [RARENA + rax], ecx
	NEXT_MEM_C_INSTR
%endif
HANDLER c_step
	CALL_CPP KIND_C_STEP, NEXT_C_INSTR

;; RV32C_BC_FUNCTION keeps the original 16-bit instruction: compressed
;; instructions without a bytecode of their own (C.NOP, C.EBREAK, Zcb, reserved
;; encodings, ...), executed by C++.
C_HANDLER c_function
	CALL_CPP KIND_C_FUNCTION, NEXT_C_INSTR

C_HANDLER c_ldw
%if XLEN == 64
	LOAD {movsxd rax, dword [RARENA + rax]}, NEXT_MEM_C_INSTR
%else
	LOAD {mov eax, dword [RARENA + rax]}, NEXT_MEM_C_INSTR
%endif

C_HANDLER c_stw
	STORE {mov dword [RARENA + rax], ecx}, NEXT_MEM_C_INSTR

%if XLEN == 64
C_HANDLER c_ldd
	LOAD {mov rax, qword [RARENA + rax]}, NEXT_MEM_C_INSTR
C_HANDLER c_std
	STORE {mov qword [RARENA + rax], rcx}, NEXT_MEM_C_INSTR
%endif
%endif ; COMPRESSED
	FLUSH_FALLBACK
	%undef CUR_HANDLER
%endmacro

;; Backward branches: a predicted indirect jump through a table indexed by the
;; branch offset lands on a stub that moves the decoder pointer by a constant.
;; The new decoder pointer then no longer waits for the offset to be loaded
;; (value speculation by way of the indirect branch predictor). The table covers
;; every non-positive 16-bit offset, so no range check is needed; offsets beyond
;; the stubs go to a generic stub.
%if COMPRESSED
%define STUB_GRAN 2
%else
%define STUB_GRAN 4
%endif
%define NSTUBS 256
%assign COLOR 0
%if COUNTING
%define BSTUB_ALIGN 64
%else
%define BSTUB_ALIGN 16
%endif
%assign SK 0
%rep NSTUBS
align BSTUB_ALIGN
bstub_%[SK]:
	sub rbx, SK * STUB_GRAN * BRSCALE
	CHECK_OVERFLOW_RBX
	DISPATCH_BLOCK
%assign SK SK+1
%endrep
align BSTUB_ALIGN
bstub_generic:
	movsx rax, word [rbx + 4]
	lea rbx, [rbx + rax * BRSCALE]
	CHECK_OVERFLOW_RBX
	DISPATCH_BLOCK

[section RELRO_SECTION]
align 8
backward_table:
%assign SK (32768 / STUB_GRAN)
%rep (32768 / STUB_GRAN) + 1
%if SK < NSTUBS
	dq bstub_%[SK]
%else
	dq bstub_generic
%endif
%assign SK SK-1
%endrep
backward_table_end equ $ - 8
__?SECT?__

%rep NCOPIES
	ALL_HANDLERS
	%assign COPY COPY+1
%endrep
%assign COLOR 0

%if WIN64
dispatch_text_end:
;; Unwind info. rsp does not move after the prologue (see "Stack frame"), so
;; one RUNTIME_FUNCTION covers the dispatch and every handler and stub after
;; it: from the end of the prologue on, all of the codes apply. Without it,
;; anything that walks the stack through a call into C++ (debuggers,
;; profilers, SEH dispatch) would take [rsp] for the return address.
;; UNWIND_CODE: offset after the instruction, then op | info << 4,
;; in reverse order. UWOP_PUSH_NONVOL = 0 (info = register number),
;; UWOP_ALLOC_SMALL = 2 (info = size / 8 - 1).
%define UW_PUSH(label, reg) db label - %[PFX]dispatch, 0 | (reg << 4)
section .xdata rdata align=4
dispatch_unwind:
	db 1                                         ; version 1, no flags
	db %[PFX]dispatch.uw_alloc - %[PFX]dispatch  ; size of the prologue
	db 9                                         ; number of codes
	db 0                                         ; no frame register
	db %[PFX]dispatch.uw_alloc - %[PFX]dispatch, 2 | ((FRAME_SIZE / 8 - 1) << 4)
	UW_PUSH(%[PFX]dispatch.uw_r15, 15)
	UW_PUSH(%[PFX]dispatch.uw_r14, 14)
	UW_PUSH(%[PFX]dispatch.uw_r13, 13)
	UW_PUSH(%[PFX]dispatch.uw_r12, 12)
	UW_PUSH(%[PFX]dispatch.uw_rsi, 6)
	UW_PUSH(%[PFX]dispatch.uw_rdi, 7)
	UW_PUSH(%[PFX]dispatch.uw_rbp, 5)
	UW_PUSH(%[PFX]dispatch.uw_rbx, 3)
	dw 0                                         ; pad to an even count
section .pdata rdata align=4
	dd %[PFX]dispatch wrt ..imagebase
	dd dispatch_text_end wrt ..imagebase
	dd dispatch_unwind wrt ..imagebase
%else
section .note.GNU-stack noalloc noexec nowrite progbits
%endif
