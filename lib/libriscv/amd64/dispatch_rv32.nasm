;; RV32 inaccurate (no instruction counting) dispatch for libriscv, AMD64 SysV.
;;
;; Assemble with: nasm -f elf64 -DCOMPRESSED=0|1 dispatch_rv32.nasm
;;
;; The bytecodes and the 8-byte decoder entries are produced by the regular
;; decoder (decode_bytecodes.cpp) and are shared with the C++ dispatch:
;;   uint8_t  bytecode, uint8_t handler, uint16_t idxend(/icount), uint32_t instr
;;
;; The dispatch table (bytecode -> handler address) is filled in by C++ from the
;; real enum values, so this file does not depend on which extensions are enabled.
;; Every bytecode without a handler here maps to rv32asm_fallback, which returns
;; to C++ so that it can execute exactly one instruction and re-enter.
;;
;; The only call out is to rv32asm_execute (FUNCTION entries), which is noexcept
;; and hands any exception back to C++, so nothing unwinds through these frames.
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
;; The register file in memory is not the problem: with memory renaming a
;; store and a dependent load cost ~1-2 cycles, once the predictor can tell
;; guest instructions apart.
[bits 64]
DEFAULT REL

%ifndef COMPRESSED
%define COMPRESSED 0
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
;;   rbx  decoder entry pointer (current instruction)
;;   rbp  integer register file (x0..x31, 4 bytes each)
;;   r12  decoder cache base (indexed by pc >> SHIFT)
;;   r13  flat arena base
;;   r14  dispatch tables (NCOPIES x 256 x 8-byte absolute addresses)
;;   r15  end of the backward-branch table (entry for offset 0)
;;   [rsp] context
;;   r8d  arena read boundary   (addr - 0x1000 < r8d is readable)
;;   r9d  initial rodata end
;;   r10d arena write boundary  (addr - r9d < r10d is writable)
;;   r11d current execute segment begin
;;   esi  current execute segment length (end - begin)
;; Scratch: rax, rcx, rdx, rdi

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

%define EXIT_STOP     0
%define EXIT_FALLBACK 1
%define EXIT_JUMP     2
%define EXIT_EXCEPTION 3

;; uint32_t rv32asm_execute(CPU*, uint8_t* handler, uint32_t instr) noexcept
;; Runs a decoded instruction handler in C++. Returns 0 to continue, 1 when the
;; execute segment went stale and 2 when the handler threw (C++ keeps it).
extern rv32asm_execute

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
%ifndef NCOPIES
%define NCOPIES 3
%endif
%ifndef COLOR_MUL
%define COLOR_MUL 0
%endif
%ifndef NO_CLASS_COLORS
%define CLASS_COLORS
%endif
;; Handler classes (used with CLASS_COLORS): 0 = control flow, 1 = ALU,
;; 2 = memory and everything else
%define CLASS_DEFAULT 1
%define CLASS_fallback 0
%define CLASS_stop 0
%define CLASS_function 2
%define CLASS_c_function 2
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
%define CLASS_c_jal 0
%define CLASS_c_jr 0
%define CLASS_c_jalr 0
%define CLASS_ldw 2
%define CLASS_ldh 2
%define CLASS_ldhu 2
%define CLASS_ldb 2
%define CLASS_ldbu 2
%define CLASS_stw 2
%define CLASS_sth 2
%define CLASS_stb 2
%define CLASS_c_ldw 2
%define CLASS_c_stw 2
%assign COPY  0
%assign HID   0
%assign COLOR 0
%define TABLE_OFFSET (COLOR * 256 * 8)

%define REG(x) dword [rbp + x * 4]

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

%macro NEXT_INSTR 0
	NEXT I4
%endmacro
%macro NEXT_C_INSTR 0
	NEXT I2
%endmacro

;; %1 = 64-bit register receiving the pc of the current instruction
%macro RECONSTRUCT_PC 1
	mov %1, rbx
	sub %1, r12
	shr %1, PC2DEC
%endmacro

;; Branch by the signed 16-bit byte offset in the instruction
%macro TAKE_BRANCH 0
	movsx rax, word [rbx + 4]
	lea rbx, [rbx + rax * BRSCALE]
	DISPATCH
%endmacro

;; Defines copy COPY of a handler. The exported symbol rv32asm_<name> is an
;; array with the address of every copy.
%macro HANDLER 1
	%assign HID HID+1
%ifdef CLASS_COLORS
	;; The color is the class of this handler, so the copy that runs the next
	;; guest instruction is selected by what kind of instruction preceded it.
	%ifdef CLASS_%1
	%assign COLOR (CLASS_%1 % NCOPIES)
	%else
	%assign COLOR (CLASS_DEFAULT % NCOPIES)
	%endif
%else
	%assign COLOR ((HID + COPY * COLOR_MUL) % NCOPIES)
%endif
	%if COPY == 0
	[section .data.rel.ro]
	align 8
	global rv32asm_%1
	rv32asm_%1:
	%assign %%i 0
	%rep NCOPIES
		dq rv32asm_%1_%[%%i]
		%assign %%i %%i+1
	%endrep
	__?SECT?__
	%endif
	align 16
	rv32asm_%1_%[COPY]:
%endmacro

section .rodata
global rv32asm_ncopies
rv32asm_ncopies: dd NCOPIES

section .text

;; uint32_t rv32asm_dispatch(AsmDispatchContext* ctx)
;; Starts executing at ctx->pc, which must be inside the execute segment.
;; Returns an EXIT_* code with ctx->pc set accordingly.
align 16
global rv32asm_dispatch
rv32asm_dispatch:
	push rbx
	push rbp
	push r12
	push r13
	push r14
	push r15
	push rdi                ; [rsp] = context
	mov r15, rdi
	mov rbp, [r15 + CTX_REGS]
	mov r12, [r15 + CTX_DECODER]
	mov r13, [r15 + CTX_ARENA]
	mov r14, [r15 + CTX_TABLE]
	mov r8d,  dword [r15 + CTX_RBOUND]
	mov r9d,  dword [r15 + CTX_RODATA]
	mov r10d, dword [r15 + CTX_WBOUND]
	mov r11d, dword [r15 + CTX_BEGIN]
	mov esi,  dword [r15 + CTX_LENGTH]
	mov eax,  dword [r15 + CTX_PC]
	lea r15, [backward_table_end]
	lea rbx, [r12 + rax * BRSCALE]
	DISPATCH

;; Leave the dispatch: eax = exit code, ctx->pc already written
exit_dispatch:
	pop rdi
	pop r15
	pop r14
	pop r13
	pop r12
	pop rbp
	pop rbx
	ret

;; Execute the current instruction in C++
do_fallback:
	RECONSTRUCT_PC rax
	mov rdi, [rsp]
	mov dword [rdi + CTX_PC], eax
	mov eax, EXIT_FALLBACK
	jmp exit_dispatch

;; Jump to eax, which is outside the current execute segment
exit_exception:
	RECONSTRUCT_PC rax
	mov rdi, [rsp]
	mov dword [rdi + CTX_PC], eax
	mov eax, EXIT_EXCEPTION
	jmp exit_dispatch

exit_jump:
	mov rdi, [rsp]
	mov dword [rdi + CTX_PC], eax
	mov eax, EXIT_JUMP
	jmp exit_dispatch

do_stop:
	RECONSTRUCT_PC rax
	add eax, 4
	mov rdi, [rsp]
	mov dword [rdi + CTX_PC], eax
	mov eax, EXIT_STOP
	jmp exit_dispatch

;; Jump to the address in eax: within the segment it is just a new
;; decoder pointer, otherwise C++ has to find the segment.
%macro JUMP_TO_EAX 0
	mov ecx, eax
	sub ecx, r11d
	cmp ecx, esi
	jae exit_jump
	lea rbx, [r12 + rax * BRSCALE]
	DISPATCH
%endmacro

;; Call the decoded handler of the current (FUNCTION) entry in C++.
;; The pinned caller-saved registers are kept on the stack, which is 16-byte
;; aligned in every handler. %1 = instruction length, %2 = NEXT macro
%macro CALL_HANDLER 2
	push r8
	push r9
	push r10
	push r11
	push rsi
	mov rdi, [rsp + 40]     ; context
	mov rdi, [rdi + CTX_CPU]
	lea rsi, [rbx + 1]      ; uint8_t& handler index
	shr rdx, 32             ; edx = original instruction
	sub rsp, 8
	call rv32asm_execute wrt ..plt
	add rsp, 8
	pop rsi
	pop r11
	pop r10
	pop r9
	pop r8
	test eax, eax
	jnz %%special
	%2
%%special:
	cmp eax, 1
	jne exit_exception
	RECONSTRUCT_PC rax      ; stale segment: continue after this instruction in C++
	add eax, %1
	jmp exit_jump
%endmacro

;; Division, RISC-V semantics (no traps)
%macro DIVISION 1   ; %1 = divu, remu, div, rem
	movzx eax, byte [rbx + 7]
	movzx ecx, byte [rbx + 6]
	movzx edi, word [rbx + 4]
	mov eax, REG(rax)
	mov ecx, REG(rcx)
	test ecx, ecx
	jz %%by_zero
%ifidn %1, div
	cmp eax, 0x80000000
	jne %%normal
	cmp ecx, -1
	je %%overflow
%%normal:
	cdq
	idiv ecx
%elifidn %1, rem
	cmp eax, 0x80000000
	jne %%normal
	cmp ecx, -1
	je %%overflow
%%normal:
	cdq
	idiv ecx
	mov eax, edx
%elifidn %1, divu
	xor edx, edx
	div ecx
%else
	xor edx, edx
	div ecx
	mov eax, edx
%endif
	mov REG(rdi), eax
	NEXT_INSTR
%%by_zero:
%ifidn %1, divu
	mov eax, -1
%elifidn %1, div
	mov eax, -1
%endif
	mov REG(rdi), eax       ; rem/remu by zero: the dividend
	NEXT_INSTR
%ifidn %1, div
%%overflow:
	mov REG(rdi), eax       ; INT_MIN / -1 = INT_MIN
	NEXT_INSTR
%elifidn %1, rem
%%overflow:
	mov dword REG(rdi), 0   ; INT_MIN % -1 = 0
	NEXT_INSTR
%endif
%endmacro

%macro ITYPE_LOAD_SRC 0   ; eax = REG(rs2), ecx = imm (unsigned 16), edx = rd
	movzx ecx, byte [rbx + 6]
	movzx edx, byte [rbx + 7]
	mov eax, REG(rcx)
%endmacro
%macro OP_LOAD 0   ; eax = REG(rs1), ecx = REG(rs2), edx = rd
	movzx eax, byte [rbx + 7]
	movzx ecx, byte [rbx + 6]
	movzx edx, word [rbx + 4]
	mov eax, REG(rax)
	mov ecx, REG(rcx)
%endmacro
%macro BRANCH 1   ; %1 = jcc taken when the branch is taken
	movzx eax, byte [rbx + 7]
	movzx ecx, byte [rbx + 6]
	mov eax, REG(rax)
	cmp eax, REG(rcx)
	%1 %%taken
	NEXT_INSTR
%%taken:
	TAKE_BRANCH
%endmacro
%macro BRANCH_BACKWARD 1   ; %1 = jcc taken when the branch is NOT taken
	movzx eax, byte [rbx + 7]
	movzx ecx, byte [rbx + 6]
	mov eax, REG(rax)
	cmp eax, REG(rcx)
	%1 %%not_taken
%ifdef NO_BSTUBS
	TAKE_BRANCH
%else
	movsx rax, word [rbx + 4]   ; <= 0 for BEQ/BNE (the rewriter makes the rest _FW)
	jmp [r15 + rax * (8 / STUB_GRAN)]
%endif
%%not_taken:
	NEXT_INSTR
%endmacro
%macro LOAD 1   ; %1 = load instruction with eax as destination
	movzx ecx, byte [rbx + 6]
	movsx eax, word [rbx + 4]
	add eax, REG(rcx)
	lea ecx, [rax - RWREAD_BEGIN]
	cmp ecx, r8d
	jae do_fallback
	movzx edx, byte [rbx + 7]
	%1
	mov REG(rdx), eax
	NEXT_INSTR
%endmacro
%macro STORE 1   ; %1 = store instruction, value in ecx
	movzx ecx, byte [rbx + 7]
	movsx eax, word [rbx + 4]
	add eax, REG(rcx)
	mov edx, eax
	sub edx, r9d
	cmp edx, r10d
	jae do_fallback
	movzx ecx, byte [rbx + 6]
	mov ecx, REG(rcx)
	%1
	NEXT_INSTR
%endmacro

%macro ALL_HANDLERS 0
	%assign HID 0
HANDLER fallback
	jmp do_fallback
HANDLER stop
	jmp do_stop

;; -== Immediate ALU, FasterItype: imm16 @4, rs2 (src) @6, rs1 (dst) @7 ==-

HANDLER addi
	rorx rax, rdx, 32       ; ax = imm, bits 16..23 = rs2, 24..31 = rs1
	movsx edi, ax
	shr eax, 16
	movzx ecx, al
	movzx eax, ah
	add edi, REG(rcx)
	mov REG(rax), edi
	NEXT_INSTR

;; FasterImmediate: rd @4, imm16 @6
HANDLER li
	movzx ecx, byte [rbx + 4]
	movsx eax, word [rbx + 6]
	mov REG(rcx), eax
	NEXT_INSTR

;; FasterMove: rs1 @4, rd @5
HANDLER mv
	movzx ecx, byte [rbx + 4]
	movzx eax, byte [rbx + 5]
	mov ecx, REG(rcx)
	mov REG(rax), ecx
	NEXT_INSTR


HANDLER slli
	ITYPE_LOAD_SRC
	movzx ecx, word [rbx + 4]
	shl eax, cl
	mov REG(rdx), eax
	NEXT_INSTR

HANDLER srli
	ITYPE_LOAD_SRC
	movzx ecx, word [rbx + 4]
	shr eax, cl
	mov REG(rdx), eax
	NEXT_INSTR

HANDLER srai
	ITYPE_LOAD_SRC
	movzx ecx, word [rbx + 4]
	sar eax, cl
	mov REG(rdx), eax
	NEXT_INSTR

HANDLER slti
	ITYPE_LOAD_SRC
	movsx ecx, word [rbx + 4]
	xor edi, edi
	cmp eax, ecx
	setl dil
	mov REG(rdx), edi
	NEXT_INSTR

HANDLER sltiu
	ITYPE_LOAD_SRC
	movsx ecx, word [rbx + 4]
	xor edi, edi
	cmp eax, ecx
	setb dil
	mov REG(rdx), edi
	NEXT_INSTR

HANDLER xori
	ITYPE_LOAD_SRC
	movsx ecx, word [rbx + 4]
	xor eax, ecx
	mov REG(rdx), eax
	NEXT_INSTR

HANDLER ori
	ITYPE_LOAD_SRC
	movsx ecx, word [rbx + 4]
	or eax, ecx
	mov REG(rdx), eax
	NEXT_INSTR

HANDLER andi
	ITYPE_LOAD_SRC
	movsx ecx, word [rbx + 4]
	and eax, ecx
	mov REG(rdx), eax
	NEXT_INSTR

HANDLER sext_b
	ITYPE_LOAD_SRC
	movsx eax, al
	mov REG(rdx), eax
	NEXT_INSTR

HANDLER sext_h
	ITYPE_LOAD_SRC
	movsx eax, ax
	mov REG(rdx), eax
	NEXT_INSTR

HANDLER bseti
	ITYPE_LOAD_SRC
	movzx ecx, word [rbx + 4]
	bts eax, ecx
	mov REG(rdx), eax
	NEXT_INSTR

HANDLER bexti
	ITYPE_LOAD_SRC
	movzx ecx, word [rbx + 4]
	shr eax, cl
	and eax, 1
	mov REG(rdx), eax
	NEXT_INSTR

;; -== Upper immediates, FasterJtype: offset24 @4..6, rd @7 ==-

HANDLER lui
	mov eax, dword [rbx + 4]
	movzx edx, byte [rbx + 7]
	shl eax, 8
	mov REG(rdx), eax
	NEXT_INSTR

HANDLER auipc
	mov eax, dword [rbx + 4]
	movzx edx, byte [rbx + 7]
	shl eax, 8
	RECONSTRUCT_PC rcx
	add eax, ecx
	mov REG(rdx), eax
	NEXT_INSTR

;; -== Register ALU, FasterOpType: rd16 @4, rs2 @6, rs1 @7 ==-


HANDLER op_add
	rorx rax, rdx, 48       ; al = rs2, ah = rs1
	shr rdx, 32             ; dx = rd
	movzx ecx, al
	movzx eax, ah
	movzx edx, dx
	mov eax, REG(rax)
	add eax, REG(rcx)
	mov REG(rdx), eax
	NEXT_INSTR

HANDLER op_sub
	OP_LOAD
	sub eax, ecx
	mov REG(rdx), eax
	NEXT_INSTR

HANDLER op_sll
	OP_LOAD
	shl eax, cl
	mov REG(rdx), eax
	NEXT_INSTR

HANDLER op_srl
	OP_LOAD
	shr eax, cl
	mov REG(rdx), eax
	NEXT_INSTR

HANDLER op_sra
	OP_LOAD
	sar eax, cl
	mov REG(rdx), eax
	NEXT_INSTR

HANDLER op_slt
	OP_LOAD
	xor edi, edi
	cmp eax, ecx
	setl dil
	mov REG(rdx), edi
	NEXT_INSTR

HANDLER op_sltu
	OP_LOAD
	xor edi, edi
	cmp eax, ecx
	setb dil
	mov REG(rdx), edi
	NEXT_INSTR

HANDLER op_xor
	OP_LOAD
	xor eax, ecx
	mov REG(rdx), eax
	NEXT_INSTR

HANDLER op_or
	OP_LOAD
	or eax, ecx
	mov REG(rdx), eax
	NEXT_INSTR

HANDLER op_and
	OP_LOAD
	and eax, ecx
	mov REG(rdx), eax
	NEXT_INSTR

HANDLER op_mul
	OP_LOAD
	imul eax, ecx
	mov REG(rdx), eax
	NEXT_INSTR

HANDLER op_div
	DIVISION div
HANDLER op_divu
	DIVISION divu
HANDLER op_rem
	DIVISION rem
HANDLER op_remu
	DIVISION remu

HANDLER function
	CALL_HANDLER 4, NEXT_INSTR

HANDLER op_zext_h
	OP_LOAD
	movzx eax, ax
	mov REG(rdx), eax
	NEXT_INSTR

HANDLER op_sh1add
	OP_LOAD
	lea eax, [rcx + rax * 2]
	mov REG(rdx), eax
	NEXT_INSTR

HANDLER op_sh2add
	OP_LOAD
	lea eax, [rcx + rax * 4]
	mov REG(rdx), eax
	NEXT_INSTR

HANDLER op_sh3add
	OP_LOAD
	lea eax, [rcx + rax * 8]
	mov REG(rdx), eax
	NEXT_INSTR

;; -== Branches, FasterItype: imm16 @4, rs2 @6, rs1 @7 ==-


;; Backward branches are mostly taken (loops): the taken path falls through

HANDLER beq
	BRANCH_BACKWARD jne
HANDLER bne
	BRANCH_BACKWARD je
HANDLER beq_fw
	BRANCH je
HANDLER bne_fw
	BRANCH jne
HANDLER blt
	BRANCH jl
HANDLER bge
	BRANCH jge
HANDLER bltu
	BRANCH jb
HANDLER bgeu
	BRANCH jae

;; -== Jumps ==-

;; FasterJtype: rd = pc + 4, pc += sext(offset24)
HANDLER jal
	RECONSTRUCT_PC rax
	movzx edx, byte [rbx + 7]
	add eax, 4
	mov REG(rdx), eax
	mov eax, dword [rbx + 4]
	shl eax, 8
	sar eax, 8
	movsxd rax, eax
	lea rbx, [rbx + rax * BRSCALE]
	DISPATCH

HANDLER fast_jal
	movsxd rax, dword [rbx + 4]
	lea rbx, [rbx + rax * BRSCALE]
	DISPATCH

HANDLER fast_call
	RECONSTRUCT_PC rax
	add eax, 4
	mov REG(1), eax
	movsxd rax, dword [rbx + 4]
	lea rbx, [rbx + rax * BRSCALE]
	DISPATCH

;; FasterItype: target = REG(rs2) + imm, rs1 = link register (0 = none)
HANDLER jalr
	movzx ecx, byte [rbx + 6]
	movsx eax, word [rbx + 4]
	add eax, REG(rcx)
	and eax, -2
%if COMPRESSED == 0
	test eax, 3
	jnz do_fallback   ; C++ raises the misaligned exception
%endif
	movzx edx, byte [rbx + 7]
	test edx, edx
	jz .no_link
	RECONSTRUCT_PC rcx
	add ecx, 4
	mov REG(rdx), ecx
.no_link:
	JUMP_TO_EAX

;; -== Loads, FasterItype: imm16 @4, rs2 (base) @6, rs1 (dst) @7 ==-


HANDLER ldw
	LOAD {mov eax, dword [r13 + rax]}
HANDLER ldh
	LOAD {movsx eax, word [r13 + rax]}
HANDLER ldhu
	LOAD {movzx eax, word [r13 + rax]}
HANDLER ldb
	LOAD {movsx eax, byte [r13 + rax]}
HANDLER ldbu
	LOAD {movzx eax, byte [r13 + rax]}

;; -== Stores, FasterItype: imm16 @4, rs2 (value) @6, rs1 (base) @7 ==-


HANDLER stw
	STORE {mov dword [r13 + rax], ecx}
HANDLER sth
	STORE {mov word [r13 + rax], cx}
HANDLER stb
	STORE {mov byte [r13 + rax], cl}

%if COMPRESSED
;; -== Compressed bytecodes, one decoder entry each ==-

;; FasterItype: rs1 = dst @7, rs2 = src @6, imm16 @4
HANDLER c_addi
	movzx ecx, byte [rbx + 6]
	movsx eax, word [rbx + 4]
	movzx edx, byte [rbx + 7]
	add eax, REG(rcx)
	mov REG(rdx), eax
	NEXT_C_INSTR

HANDLER c_mv
	movzx ecx, byte [rbx + 4]
	movzx edx, byte [rbx + 5]
	mov eax, REG(rcx)
	mov REG(rdx), eax
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
	movsx eax, word [rbx + 4]
	and REG(rdx), eax
	NEXT_C_INSTR

HANDLER c_add
	movzx ecx, byte [rbx + 6]
	movzx edx, byte [rbx + 7]
	mov eax, REG(rcx)
	add REG(rdx), eax
	NEXT_C_INSTR

HANDLER c_xor
	movzx ecx, byte [rbx + 6]
	movzx edx, byte [rbx + 7]
	mov eax, REG(rcx)
	xor REG(rdx), eax
	NEXT_C_INSTR

HANDLER c_or
	movzx ecx, byte [rbx + 6]
	movzx edx, byte [rbx + 7]
	mov eax, REG(rcx)
	or REG(rdx), eax
	NEXT_C_INSTR

HANDLER c_bnez
	movzx eax, byte [rbx + 7]
	cmp REG(rax), 0
	jne .taken
	NEXT_C_INSTR
.taken:
	TAKE_BRANCH

HANDLER c_beqz
	movzx eax, byte [rbx + 7]
	cmp REG(rax), 0
	je .taken
	NEXT_C_INSTR
.taken:
	TAKE_BRANCH

HANDLER c_jmp
	TAKE_BRANCH

;; RV32: C.JAL
HANDLER c_jal
	RECONSTRUCT_PC rax
	add eax, 2
	mov REG(1), eax
	TAKE_BRANCH

;; instr = register index
HANDLER c_jr
	mov ecx, dword [rbx + 4]
	mov eax, REG(rcx)
	and eax, -2
	JUMP_TO_EAX

HANDLER c_jalr
	mov ecx, dword [rbx + 4]
	mov eax, REG(rcx)
	and eax, -2
	RECONSTRUCT_PC rcx
	add ecx, 2
	mov REG(1), ecx
	JUMP_TO_EAX

HANDLER c_function
	CALL_HANDLER 2, NEXT_C_INSTR

HANDLER c_ldw
	movzx ecx, byte [rbx + 6]
	movsx eax, word [rbx + 4]
	add eax, REG(rcx)
	lea ecx, [rax - RWREAD_BEGIN]
	cmp ecx, r8d
	jae do_fallback
	movzx edx, byte [rbx + 7]
	mov eax, dword [r13 + rax]
	mov REG(rdx), eax
	NEXT_C_INSTR

HANDLER c_stw
	movzx ecx, byte [rbx + 7]
	movsx eax, word [rbx + 4]
	add eax, REG(rcx)
	mov edx, eax
	sub edx, r9d
	cmp edx, r10d
	jae do_fallback
	movzx ecx, byte [rbx + 6]
	mov ecx, REG(rcx)
	mov dword [r13 + rax], ecx
	NEXT_C_INSTR
%endif ; COMPRESSED

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
%assign SK 0
%rep NSTUBS
align 16
bstub_%[SK]:
	sub rbx, SK * STUB_GRAN * BRSCALE
	DISPATCH
%assign SK SK+1
%endrep
align 16
bstub_generic:
	movsx rax, word [rbx + 4]
	lea rbx, [rbx + rax * BRSCALE]
	DISPATCH

[section .data.rel.ro]
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

section .note.GNU-stack noalloc noexec nowrite progbits
