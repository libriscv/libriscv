#!/usr/bin/env python3
"""
RV64 dispatch for libriscv on WebAssembly (wasm32, Emscripten).

Generates LLVM WebAssembly assembly (.s) with two functions:
  rv64wasm_dispatch(ctx)   CPU::simulate_inaccurate (no instruction counting)
  rv64wasmc_dispatch(ctx)  CPU::simulate (counts like cpu_dispatch.cpp)
and a C++ header with the layout and bytecode numbering this file assumes,
which wasm_dispatch.cpp checks with static_asserts.

This is the WebAssembly sibling of amd64/dispatch_rv.nasm, and follows it
handler for handler: the same bytecodes have fast paths, the same decoder
entry fields are read, and everything else goes to C++ (rv64wasm[c]_call,
see asm_dispatch_glue.hpp) in exactly the same way, so that both dispatches
count instructions and stop at the same places.

Why a generator: WebAssembly control flow is structured, and LLVM's assembler
only takes branch targets as relative depths. The generator lets the handlers
use names, and resolves the depths when it writes the file.

The structure of each function:

  block $ret                 return $code (counting: store the counters)
   loop $outer               re-enter the dispatch loop
    block $goto              continue at the guest address in $pc
     block $call             call C++ with $kind, then continue
      block $exit_pc         write $pc to ctx->pc and return $code
       loop $L               the dispatch loop
        block $h_N .. $h_0
          br_table (bytecode) $h_0 .. $h_N
        end $h_0: handler 0 ... br $L
        ...

How it differs from the native dispatch, and why:
 - One dispatch site. WebAssembly has no indirect jumps, only br_table (a jump
   table) and tail calls. V8 compiles br_table to a jump table, while a chain
   of return_call_indirect handlers (tailcall_dispatch.cpp with -mtail-call)
   measured ~4x slower in V8: every call has a frame, a signature check and
   a table bounds check. The handlers return to the dispatch with a
   conditional branch, which made it far more predictable (see dispatch()).
 - Tier-up. V8 has no on-stack replacement for WebAssembly: a function that
   never returns keeps running its baseline (Liftoff) code, which here is
   ~2x slower than the optimized code. The
   dispatch therefore returns to C++ (EXIT_YIELD) when a budget of taken
   backward branches and jumps runs out, and C++ enters it again (in the
   optimized code, once V8 has compiled it). C++ doubles the budget every
   time, so this costs nothing after the first few seconds.
 - FUNCTION entries (instructions without a bytecode: FP compares and
   conversions, AMOs, ...) call their handler directly, as the C++ dispatch
   does, instead of going through asm_call(), whose try/catch is a trip
   through JavaScript with Emscripten's default exception handling.
 - FMADD calls fma()/fmaf() from libc: WebAssembly has no fused multiply-add
   that is guaranteed to round once.
"""
import argparse
import sys

# ---------------------------------------------------------------------------
# Configuration-dependent constants

# Exit codes and C++ call kinds: must match asm_dispatch_glue.hpp
EXIT_STOP = 0
EXIT_REENTER = 1
EXIT_JUMP = 2
EXIT_EXCEPTION = 3
CALL_NEXT = 4
CALL_GOTO = 5
EXIT_OVERFLOW = 6
EXIT_YIELD = 7

KIND_FUNCTION = 0
KIND_C_FUNCTION = 1
KIND_SYSCALL = 2
KIND_DYNCALL = 3
KIND_SYSTEM = 4
KIND_FUNCBLOCK = 5
KIND_STEP = 6
KIND_INVALID = 7
KIND_TRANSLATOR = 8
KIND_ASMJIT = 9
KIND_LIVEPATCH = 10
KIND_C_STEP = 11

# struct AsmDispatchContext on wasm32 (pointers are 4 bytes)
CTX = {
    'regs': 0, 'decoder': 4, 'begin': 8, 'length': 16, 'arena': 24,
    'read_boundary': 32, 'rodata_end': 40, 'write_boundary': 48,
    'table': 56, 'pc': 64, 'cpu': 72, 'counter': 80, 'max_counter': 88,
    'enter_count': 96, 'budget': 100, 'call': 104,
    'handlers': 108, 'stale': 112, 'counter_ptr': 116, 'instr': 120,
}
# Registers<8>::pc
PCOFF = 256
# Registers<8>: x0..x31, pc, fcsr, f0..f31
FPOFF = 272
RWREAD_BEGIN = 0x1000


def bytecode_list(compressed, translator, asmjit):
    """The bytecode enum in threaded_bytecodes.hpp, for an RV64 build."""
    bc = ['RV32I_BC_INVALID',
          'RV32I_BC_ADDI', 'RV32I_BC_LI', 'RV32I_BC_MV',
          'RV32I_BC_SLLI', 'RV32I_BC_SLTI', 'RV32I_BC_SLTIU', 'RV32I_BC_XORI',
          'RV32I_BC_SRLI', 'RV32I_BC_SRAI', 'RV32I_BC_ORI', 'RV32I_BC_ANDI',
          'RV32I_BC_LUI', 'RV32I_BC_AUIPC',
          'RV32I_BC_LDB', 'RV32I_BC_LDBU', 'RV32I_BC_LDH', 'RV32I_BC_LDHU', 'RV32I_BC_LDW',
          'RV32I_BC_STB', 'RV32I_BC_STH', 'RV32I_BC_STW',
          'RV32I_BC_LDWU', 'RV32I_BC_LDD', 'RV32I_BC_STD',
          'RV32I_BC_BEQ', 'RV32I_BC_BNE', 'RV32I_BC_BLT', 'RV32I_BC_BGE',
          'RV32I_BC_BLTU', 'RV32I_BC_BGEU', 'RV32I_BC_BEQ_FW', 'RV32I_BC_BNE_FW',
          'RV32I_BC_JAL', 'RV32I_BC_JALR', 'RV32I_BC_FAST_JAL', 'RV32I_BC_FAST_CALL',
          'RV32I_BC_OP_ADD', 'RV32I_BC_OP_SUB', 'RV32I_BC_OP_SLL', 'RV32I_BC_OP_SLT',
          'RV32I_BC_OP_SLTU', 'RV32I_BC_OP_XOR', 'RV32I_BC_OP_SRL', 'RV32I_BC_OP_OR',
          'RV32I_BC_OP_AND', 'RV32I_BC_OP_MUL', 'RV32I_BC_OP_DIV', 'RV32I_BC_OP_DIVU',
          'RV32I_BC_OP_REM', 'RV32I_BC_OP_REMU', 'RV32I_BC_OP_SRA', 'RV32I_BC_OP_ZEXT_H',
          'RV32I_BC_OP_SH1ADD', 'RV32I_BC_OP_SH2ADD', 'RV32I_BC_OP_SH3ADD',
          'RV32I_BC_SEXT_B', 'RV32I_BC_SEXT_H', 'RV32I_BC_BSETI', 'RV32I_BC_BEXTI',
          'RV64I_BC_ADDIW', 'RV64I_BC_SLLIW', 'RV64I_BC_SRLIW', 'RV64I_BC_SRAIW',
          'RV64I_BC_OP_ADDW', 'RV64I_BC_OP_SUBW', 'RV64I_BC_OP_MULW',
          'RV64I_BC_OP_ADD_UW', 'RV64I_BC_OP_SH1ADD_UW', 'RV64I_BC_OP_SH2ADD_UW']
    if compressed:
        bc += ['RV32C_BC_ADDI', 'RV32C_BC_LI', 'RV32C_BC_MV', 'RV32C_BC_SLLI',
               'RV32C_BC_BEQZ', 'RV32C_BC_BNEZ', 'RV32C_BC_JMP', 'RV32C_BC_JR',
               'RV32C_BC_JAL_ADDIW', 'RV32C_BC_JALR', 'RV32C_BC_LDD', 'RV32C_BC_STD',
               'RV32C_BC_LDW', 'RV32C_BC_STW', 'RV32C_BC_SRLI', 'RV32C_BC_ANDI',
               'RV32C_BC_ADD', 'RV32C_BC_XOR', 'RV32C_BC_OR', 'RV32C_BC_LUI',
               'RV32C_BC_FLD', 'RV32C_BC_FSD', 'RV32C_BC_FLW', 'RV32C_BC_FSW',
               'RV32C_BC_SRAI', 'RV32C_BC_SUB', 'RV32C_BC_AND', 'RV32C_BC_SUBW',
               'RV32C_BC_ADDW', 'RV32C_BC_FUNCTION']
    bc += ['RV32I_BC_SYSCALL', 'RV32I_BC_DYNCALL', 'RV32I_BC_STOP',
           'RV32F_BC_FLW', 'RV32F_BC_FLD', 'RV32F_BC_FSW', 'RV32F_BC_FSD',
           'RV32F_BC_FADD', 'RV32F_BC_FSUB', 'RV32F_BC_FMUL', 'RV32F_BC_FDIV',
           'RV32F_BC_FMADD', 'RV32I_BC_FUNCTION', 'RV32I_BC_FUNCBLOCK']
    if translator:
        bc += ['RV32I_BC_TRANSLATOR']
    if asmjit:
        bc += ['RV32I_BC_ASMJIT']
    bc += ['RV32I_BC_LIVEPATCH', 'RV32I_BC_SYSTEM']
    assert len(bc) <= 256
    return bc


def handler_map(compressed, nanboxing, fcsr):
    """bytecode -> handler name (like ASMDISPATCH_FILL in asm_dispatch.cpp).
    Bytecodes that are not listed go to the fallback handler."""
    m = {
        'RV32I_BC_INVALID': 'fallback', 'RV32I_BC_STOP': 'stop',
        'RV32I_BC_ADDI': 'addi', 'RV32I_BC_LI': 'li', 'RV32I_BC_MV': 'mv',
        'RV32I_BC_SLLI': 'slli', 'RV32I_BC_SRLI': 'srli', 'RV32I_BC_SRAI': 'srai',
        'RV32I_BC_SLTI': 'slti', 'RV32I_BC_SLTIU': 'sltiu', 'RV32I_BC_XORI': 'xori',
        'RV32I_BC_ORI': 'ori', 'RV32I_BC_ANDI': 'andi',
        'RV32I_BC_SEXT_B': 'sext_b', 'RV32I_BC_SEXT_H': 'sext_h',
        'RV32I_BC_BSETI': 'bseti', 'RV32I_BC_BEXTI': 'bexti',
        'RV32I_BC_LUI': 'lui', 'RV32I_BC_AUIPC': 'auipc',
        'RV32I_BC_OP_ADD': 'op_add', 'RV32I_BC_OP_SUB': 'op_sub',
        'RV32I_BC_OP_SLL': 'op_sll', 'RV32I_BC_OP_SRL': 'op_srl', 'RV32I_BC_OP_SRA': 'op_sra',
        'RV32I_BC_OP_SLT': 'op_slt', 'RV32I_BC_OP_SLTU': 'op_sltu',
        'RV32I_BC_OP_XOR': 'op_xor', 'RV32I_BC_OP_OR': 'op_or', 'RV32I_BC_OP_AND': 'op_and',
        'RV32I_BC_OP_MUL': 'op_mul', 'RV32I_BC_OP_ZEXT_H': 'op_zext_h',
        'RV32I_BC_OP_DIV': 'op_div', 'RV32I_BC_OP_DIVU': 'op_divu',
        'RV32I_BC_OP_REM': 'op_rem', 'RV32I_BC_OP_REMU': 'op_remu',
        'RV32I_BC_OP_SH1ADD': 'op_sh1add', 'RV32I_BC_OP_SH2ADD': 'op_sh2add',
        'RV32I_BC_OP_SH3ADD': 'op_sh3add',
        'RV32I_BC_FUNCTION': 'function', 'RV32I_BC_FUNCBLOCK': 'funcblock',
        'RV32I_BC_SYSCALL': 'syscall', 'RV32I_BC_DYNCALL': 'dyncall',
        'RV32I_BC_SYSTEM': 'system', 'RV32I_BC_LIVEPATCH': 'livepatch',
        'RV32I_BC_TRANSLATOR': 'translator', 'RV32I_BC_ASMJIT': 'asmjit',
        'RV32I_BC_BEQ': 'beq', 'RV32I_BC_BNE': 'bne',
        'RV32I_BC_BEQ_FW': 'beq_fw', 'RV32I_BC_BNE_FW': 'bne_fw',
        'RV32I_BC_BLT': 'blt', 'RV32I_BC_BGE': 'bge',
        'RV32I_BC_BLTU': 'bltu', 'RV32I_BC_BGEU': 'bgeu',
        'RV32I_BC_JAL': 'jal', 'RV32I_BC_FAST_JAL': 'fast_jal',
        'RV32I_BC_FAST_CALL': 'fast_call', 'RV32I_BC_JALR': 'jalr',
        'RV32I_BC_LDW': 'ldw', 'RV32I_BC_LDH': 'ldh', 'RV32I_BC_LDHU': 'ldhu',
        'RV32I_BC_LDB': 'ldb', 'RV32I_BC_LDBU': 'ldbu',
        'RV32I_BC_STW': 'stw', 'RV32I_BC_STH': 'sth', 'RV32I_BC_STB': 'stb',
        'RV32F_BC_FLD': 'fld', 'RV32F_BC_FSW': 'fsw', 'RV32F_BC_FSD': 'fsd',
        # NaN-boxing and FCSR emulation are left to C++ (CPU::step_one)
        'RV32F_BC_FLW': 'step' if nanboxing else 'flw',
        'RV64I_BC_ADDIW': 'addiw', 'RV64I_BC_SLLIW': 'slliw',
        'RV64I_BC_SRLIW': 'srliw', 'RV64I_BC_SRAIW': 'sraiw',
        'RV64I_BC_OP_ADDW': 'op_addw', 'RV64I_BC_OP_SUBW': 'op_subw',
        'RV64I_BC_OP_MULW': 'op_mulw', 'RV64I_BC_OP_ADD_UW': 'op_add_uw',
        'RV64I_BC_OP_SH1ADD_UW': 'op_sh1add_uw', 'RV64I_BC_OP_SH2ADD_UW': 'op_sh2add_uw',
        'RV32I_BC_LDWU': 'ldwu', 'RV32I_BC_LDD': 'ldd', 'RV32I_BC_STD': 'std',
    }
    fp_fast = not nanboxing and not fcsr
    for bc, h in (('RV32F_BC_FADD', 'fadd'), ('RV32F_BC_FSUB', 'fsub'),
                  ('RV32F_BC_FMUL', 'fmul'), ('RV32F_BC_FDIV', 'fdiv'),
                  ('RV32F_BC_FMADD', 'fmadd')):
        m[bc] = h if fp_fast else 'step'
    if compressed:
        m.update({
            'RV32C_BC_ADDI': 'c_addi', 'RV32C_BC_LI': 'c_addi', 'RV32C_BC_MV': 'c_mv',
            'RV32C_BC_SLLI': 'c_slli', 'RV32C_BC_SRLI': 'c_srli', 'RV32C_BC_ANDI': 'c_andi',
            'RV32C_BC_ADD': 'c_add', 'RV32C_BC_XOR': 'c_xor', 'RV32C_BC_OR': 'c_or',
            'RV32C_BC_BNEZ': 'c_bnez', 'RV32C_BC_BEQZ': 'c_beqz', 'RV32C_BC_JMP': 'c_jmp',
            'RV32C_BC_JAL_ADDIW': 'c_addiw',
            'RV32C_BC_JR': 'c_jr', 'RV32C_BC_JALR': 'c_jalr',
            'RV32C_BC_FUNCTION': 'c_function',
            'RV32C_BC_LDW': 'c_ldw', 'RV32C_BC_STW': 'c_stw',
            'RV32C_BC_LUI': 'c_lui', 'RV32C_BC_FLD': 'c_fld', 'RV32C_BC_FSD': 'c_fsd',
            'RV32C_BC_SRAI': 'c_srai', 'RV32C_BC_SUB': 'c_sub', 'RV32C_BC_AND': 'c_and',
            'RV32C_BC_LDD': 'c_ldd', 'RV32C_BC_STD': 'c_std',
            'RV32C_BC_SUBW': 'c_subw', 'RV32C_BC_ADDW': 'c_addw',
            # RV32C_BC_FLW/FSW are RV32-only encodings: the fallback
        })
    return m


# ---------------------------------------------------------------------------
# A function body with named labels

class Func:
    def __init__(self, name, params, results, local_types):
        self.name = name
        self.params = params            # list of (name, type)
        self.results = results
        self.locals = {}
        self.types = []
        for i, (n, t) in enumerate(params):
            self.locals[n] = i
        for n, t in local_types:
            self.locals[n] = len(self.locals)
            self.types.append(t)
        self.lines = []
        self.stack = []                 # open labels (None = anonymous)

    def op(self, *parts):
        self.lines.append('\t' + ' '.join(str(p) for p in parts))

    def comment(self, text):
        self.lines.append('\t# ' + text)

    def label_depth(self, name):
        for depth, n in enumerate(reversed(self.stack)):
            if n == name:
                return depth
        raise KeyError(f'label {name} is not open in {self.name}')

    # Structured control flow
    def block(self, name=None):
        self.op('block')
        self.stack.append(name)

    def loop(self, name=None):
        self.op('loop')
        self.stack.append(name)

    def if_(self, result=None):
        self.op('if', result) if result else self.op('if')
        self.stack.append(None)

    def else_(self):
        self.op('else')

    def end(self, kind):
        self.op('end_' + kind)
        self.stack.pop()

    def br(self, name):
        self.op('br', self.label_depth(name))

    def br_if(self, name):
        self.op('br_if', self.label_depth(name))

    def br_table(self, names):
        depths = [self.label_depth(n) for n in names]
        # The last entry is the default
        self.op('br_table', '{' + ', '.join(str(d) for d in depths) + '}')

    # Locals
    def get(self, n):
        self.op('local.get', self.locals[n])

    def set(self, n):
        self.op('local.set', self.locals[n])

    def tee(self, n):
        self.op('local.tee', self.locals[n])

    def emit(self, out):
        out.append(f'\t.section\t.text.{self.name},"",@')
        out.append(f'\t.hidden\t{self.name}')
        out.append(f'\t.globl\t{self.name}')
        out.append(f'\t.type\t{self.name},@function')
        out.append(f'{self.name}:')
        ps = ', '.join(t for _, t in self.params)
        rs = ', '.join(self.results)
        out.append(f'\t.functype\t{self.name} ({ps}) -> ({rs})')
        if self.types:
            out.append('\t.local\t' + ', '.join(self.types))
        out.extend(self.lines)
        out.append('\tend_function')
        assert not self.stack, f'unbalanced labels in {self.name}: {self.stack}'


# ---------------------------------------------------------------------------
# The dispatch

class Dispatch:
    def __init__(self, counting, compressed, bytecodes, hmap):
        self.counting = counting
        self.compressed = compressed
        self.shift = 1 if compressed else 2
        # One 8-byte decoder entry per 2^SHIFT bytes of code
        self.I4 = 8 * (4 >> self.shift)  # decoder bytes of a 4-byte instruction
        self.I2 = 8                      # ... of a 2-byte instruction
        self.PC2DEC = 3 - self.shift     # decoder offset = pc << PC2DEC
        self.bytecodes = bytecodes
        self.hmap = hmap
        self.prefix = 'rv64wasmc_' if counting else 'rv64wasm_'
        local_types = [
            ('d', 'i32'),       # current decoder entry
            ('regs', 'i32'),    # register file
            ('dec', 'i32'),     # decoder cache base (entry of pc 0)
            ('arena', 'i32'),   # flat arena base
            ('rbound', 'i64'),  # addr - 0x1000 < rbound is readable
            ('rodata', 'i64'),  # initial rodata end
            ('wbound', 'i64'),  # addr - rodata < wbound is writable
            ('begin', 'i64'),   # execute segment begin
            ('length', 'i64'),  # execute segment length
            ('counter', 'i64'),
            ('max', 'i64'),
            ('budget', 'i32'),  # taken backward branches and jumps until EXIT_YIELD
            ('pc', 'i64'),      # guest address for $goto and $exit_pc
            ('code', 'i32'),    # exit code for $ret and $exit_pc
            ('kind', 'i32'),    # KIND_* for $call
            ('a', 'i64'),
            ('b', 'i64'),
            ('t', 'i32'),
            ('bc', 'i32'),      # the bytecode of $d, which the dispatch branches on
        ]
        self.f = Func(self.prefix + 'dispatch', [('ctx', 'i32')], ['i32'], local_types)
        self.cur_c = False      # the current handler is for a 2-byte instruction

    # -- Decoder entry fields (FasterItype: imm16 @4, rs2 @6, rs1 @7) --
    def u8(self, off):
        f = self.f
        f.get('d')
        f.op('i32.load8_u', off)

    def reg_addr(self, off, width='u8'):
        """Push the address of the register whose index is at entry+off."""
        f = self.f
        f.get('d')
        f.op('i32.load8_u' if width == 'u8' else 'i32.load16_u', off)
        f.op('i32.const', 3)
        f.op('i32.shl')
        f.get('regs')
        f.op('i32.add')

    def reg(self, off, width='u8'):
        """Push the 64-bit register whose index is at entry+off."""
        self.reg_addr(off, width)
        self.f.op('i64.load', 0)

    def reg32(self, off):
        """Push the low 32 bits of the register whose index is at entry+off."""
        self.reg_addr(off)
        self.f.op('i32.load', 0)

    def imm16s(self, off=4, t='i64'):
        self.f.get('d')
        self.f.op(f'{t}.load16_s', off)

    def imm16u(self, off=4, t='i64'):
        self.f.get('d')
        self.f.op(f'{t}.load16_u', off)

    def pc_of_d(self):
        """Push the guest pc of the current decoder entry (i64)."""
        f = self.f
        f.get('d')
        f.get('dec')
        f.op('i32.sub')
        f.op('i32.const', self.PC2DEC)
        f.op('i32.shr_u')
        f.op('i64.extend_i32_u')

    def ctx_load(self, field, t='i64'):
        f = self.f
        f.get('ctx')
        f.op(f'{t}.load', CTX[field])

    def ctx_store_local(self, field, local, t='i64'):
        f = self.f
        f.get('ctx')
        f.get(local)
        f.op(f'{t}.store', CTX[field])

    # -- Dispatch --
    def dispatch(self):
        """Dispatch the entry at $d (not a block start).

        The bytecode is loaded here, at the end of the handler, rather than at
        the dispatch site, so that the load issues with the rest of the handler.

        Every handler then returns to the dispatch with a conditional branch
        (bytecode < 128, which always holds: there are fewer bytecodes) instead
        of an unconditional one. That made the shared dispatch jump far more
        predictable: on Zen 4 in V8, CoreMark went from 4-8 % of the dispatch
        jumps mispredicted to ~0.3 %, and 1850 to ~2100 iterations/s, for one
        fused compare-and-branch per instruction. Branching back to the start
        of the handler when the next bytecode is the same one (like NEXT_SELF
        in the native dispatch) predicted just as well, but every handler is
        then a loop, and V8 adds a stack check to every loop header."""
        f = self.f
        f.get('d')
        f.op('i32.load8_u', 0)
        f.tee('bc')
        f.op('i32.const', len(self.bytecodes))
        f.op('i32.lt_u')
        f.br_if('L')
        f.op('unreachable')

    def count_block(self):
        """COUNTING: add the instruction count of the block that begins at $d."""
        if not self.counting:
            return
        f = self.f
        f.get('counter')
        if self.compressed:
            self.u8(3)                  # icount
        else:
            f.get('d')
            f.op('i32.load16_u', 2)     # idxend
            f.op('i32.const', 1)
            f.op('i32.add')
        f.op('i64.extend_i32_u')
        f.op('i64.add')
        f.set('counter')

    def dispatch_block(self):
        """Dispatch the entry at $d, which begins a block."""
        self.count_block()
        self.dispatch()

    def advance(self, n):
        f = self.f
        f.get('d')
        f.op('i32.const', n)
        f.op('i32.add')
        f.set('d')

    def next(self, n=None):
        if n is None:
            n = self.I2 if self.cur_c else self.I4
        self.advance(n)
        self.dispatch()

    def next_block(self, n=None):
        if n is None:
            n = self.I2 if self.cur_c else self.I4
        self.advance(n)
        self.dispatch_block()

    def check_d(self):
        """At a taken branch that may go backwards, or a jump to $d: stop when the
        counter has reached the max (COUNTING), and yield when the budget has
        run out. Both leave with the pc of $d, which then begins a block."""
        f = self.f
        if self.counting:
            f.get('counter')
            f.get('max')
            f.op('i64.ge_u')
            f.if_()
            self.pc_of_d()
            f.set('pc')
            f.op('i32.const', EXIT_OVERFLOW)
            f.set('code')
            f.br('exit_pc')
            f.end('if')
        self.check_budget(lambda: (self.pc_of_d(), f.set('pc')))

    def check_budget(self, set_pc):
        f = self.f
        f.get('budget')
        f.op('i32.const', 1)
        f.op('i32.sub')
        f.tee('budget')
        f.op('i32.eqz')
        f.if_()
        set_pc()
        f.op('i32.const', EXIT_YIELD)
        f.set('code')
        f.br('exit_pc')
        f.end('if')

    def take_branch(self, check):
        """Branch by the signed 16-bit byte offset in the entry."""
        f = self.f
        f.get('d')
        f.get('d')
        f.op('i32.load16_s', 4)
        f.op('i32.const', self.PC2DEC)
        f.op('i32.shl')
        f.op('i32.add')
        f.set('d')
        if check:
            self.check_d()
        self.dispatch_block()

    def jump_to_pc(self):
        """Jump to the guest address in $pc (JUMP_TO_EAX)."""
        f = self.f
        if self.counting:
            f.get('counter')
            f.get('max')
            f.op('i64.ge_u')
            f.if_()
            f.op('i32.const', EXIT_OVERFLOW)
            f.set('code')
            f.br('exit_pc')
            f.end('if')
        self.check_budget(lambda: None)
        # Outside the execute segment: C++ finds the right one
        f.get('pc')
        f.get('begin')
        f.op('i64.sub')
        f.get('length')
        f.op('i64.ge_u')
        f.if_()
        f.op('i32.const', EXIT_JUMP)
        f.set('code')
        f.br('exit_pc')
        f.end('if')
        f.get('dec')
        f.get('pc')
        f.op('i32.wrap_i64')
        f.op('i32.const', self.PC2DEC)
        f.op('i32.shl')
        f.op('i32.add')
        f.set('d')
        self.dispatch_block()

    def call_cpp(self, kind):
        f = self.f
        f.op('i32.const', kind)
        f.set('kind')
        f.br('call')

    def fallback_if(self):
        """The condition on the stack sends this instruction to C++
        (CPU::step_one), which also raises whatever exception is due."""
        f = self.f
        f.if_()
        self.call_cpp(KIND_C_STEP if self.cur_c else KIND_STEP)
        f.end('if')

    # -- Handler building blocks --
    def alu_imm(self, op, imm='s'):
        """REG(@7) = REG(@6) op imm16"""
        f = self.f
        self.reg_addr(7)
        self.reg(6)
        self.imm16s() if imm == 's' else self.imm16u()
        f.op(op)
        f.op('i64.store', 0)
        self.next()

    def alu_reg(self, op):
        """FasterOpType: REG(rd16 @4) = REG(@7) op REG(@6)"""
        f = self.f
        self.reg_addr(4, 'u16')
        self.reg(7)
        self.reg(6)
        f.op(op)
        f.op('i64.store', 0)
        self.next()

    def alu_reg32(self, op):
        f = self.f
        self.reg_addr(4, 'u16')
        self.reg32(7)
        self.reg32(6)
        f.op(op)
        f.op('i64.extend_i32_s')
        f.op('i64.store', 0)
        self.next()

    def shadd(self, shift, uw=False):
        """REG(rd16) = (REG(@7) << shift) + REG(@6)"""
        f = self.f
        self.reg_addr(4, 'u16')
        self.reg_addr(7)
        f.op('i64.load32_u' if uw else 'i64.load', 0)
        f.op('i64.const', shift)
        f.op('i64.shl')
        self.reg(6)
        f.op('i64.add')
        f.op('i64.store', 0)
        self.next()

    def division(self, kind):
        f = self.f
        self.reg_addr(4, 'u16')
        self.reg(7)
        f.set('a')
        self.reg(6)
        f.tee('b')
        f.op('i64.eqz')
        f.if_('i64')
        if kind in ('div', 'divu'):
            f.op('i64.const', -1)
        else:
            f.get('a')                  # rem/remu by zero: the dividend
        f.else_()
        if kind in ('div', 'rem'):
            # Division by -1 is negation (MIN / -1 = MIN), and the remainder 0
            f.get('b')
            f.op('i64.const', -1)
            f.op('i64.eq')
            f.if_('i64')
            if kind == 'div':
                f.op('i64.const', 0)
                f.get('a')
                f.op('i64.sub')
            else:
                f.op('i64.const', 0)
            f.else_()
            f.get('a')
            f.get('b')
            f.op('i64.div_s' if kind == 'div' else 'i64.rem_s')
            f.end('if')
        else:
            f.get('a')
            f.get('b')
            f.op('i64.div_u' if kind == 'divu' else 'i64.rem_u')
        f.end('if')
        f.op('i64.store', 0)
        self.next()

    def branch(self, cmp, check):
        """FasterItype: REG(@7) cmp REG(@6), imm16 @4. The fall-through begins
        a block."""
        f = self.f
        self.reg(7)
        self.reg(6)
        f.op(cmp)
        f.if_()
        self.take_branch(check)
        f.end('if')
        self.next_block()

    def read_address(self, base_off):
        """$a = REG(@base_off) + imm16, or the fallback when it is not readable."""
        f = self.f
        self.reg(base_off)
        self.imm16s()
        f.op('i64.add')
        f.tee('a')
        f.op('i64.const', RWREAD_BEGIN)
        f.op('i64.sub')
        f.get('rbound')
        f.op('i64.ge_u')
        self.fallback_if()

    def write_address(self, base_off):
        f = self.f
        self.reg(base_off)
        self.imm16s()
        f.op('i64.add')
        f.tee('a')
        f.get('rodata')
        f.op('i64.sub')
        f.get('wbound')
        f.op('i64.ge_u')
        self.fallback_if()

    def host_addr(self):
        """Push the host address of the guest address in $a."""
        f = self.f
        f.get('arena')
        f.get('a')
        f.op('i32.wrap_i64')
        f.op('i32.add')

    def load(self, op):
        """FasterItype: imm16 @4, rs2 (base) @6, rs1 (dst) @7"""
        f = self.f
        self.read_address(6)
        self.reg_addr(7)
        self.host_addr()
        f.op(op, 0)
        f.op('i64.store', 0)
        self.next()

    def store(self, op):
        """FasterItype: imm16 @4, rs2 (value) @6, rs1 (base) @7"""
        f = self.f
        self.write_address(7)
        self.host_addr()
        self.reg(6)
        f.op(op, 0)
        self.next()

    def fp_load(self, width):
        f = self.f
        self.read_address(6)
        self.reg_addr(7)
        self.host_addr()
        # Integer loads and stores: the bits are copied as they are.
        # Without NaN-boxing a single-precision load writes only the low half.
        f.op('i32.load' if width == 32 else 'i64.load', 0)
        f.op('i32.store' if width == 32 else 'i64.store', FPOFF)
        self.next()

    def fp_store(self, width):
        f = self.f
        self.write_address(7)
        self.host_addr()
        self.reg_addr(6)
        f.op('i32.load' if width == 32 else 'i64.load', FPOFF)
        f.op('i32.store' if width == 32 else 'i64.store', 0)
        self.next()

    def fp_arith(self, op):
        """FasterFloatType: func @4 (0 = single, 1 = double), rd @5, rs2 @6, rs1 @7"""
        f = self.f
        self.u8(4)
        f.op('i32.const', 1)
        f.op('i32.and')
        f.if_()
        for t in ('f64', 'f32'):
            self.reg_addr(5)
            self.reg_addr(7)
            f.op(f'{t}.load', FPOFF)
            self.reg_addr(6)
            f.op(f'{t}.load', FPOFF)
            f.op(f'{t}.{op}')
            f.op(f'{t}.store', FPOFF)    # f32: only the low half
            self.next()
            if t == 'f64':
                f.else_()
        f.end('if')
        f.op('unreachable')

    def creg_op(self, op):
        """REG(@7) = REG(@7) op REG(@6)"""
        f = self.f
        self.reg_addr(7)
        f.tee('t')
        f.get('t')
        f.op('i64.load', 0)
        self.reg(6)
        f.op(op)
        f.op('i64.store', 0)
        self.next()

    def creg_imm(self, op, imm):
        """REG(@7) = REG(@7) op imm16"""
        f = self.f
        self.reg_addr(7)
        f.tee('t')
        f.get('t')
        f.op('i64.load', 0)
        self.imm16s() if imm == 's' else self.imm16u()
        f.op(op)
        f.op('i64.store', 0)
        self.next()

    def creg_op32(self, op, rhs):
        f = self.f
        self.reg_addr(7)
        f.tee('t')
        f.get('t')
        f.op('i32.load', 0)
        if rhs == 'reg':
            self.reg32(6)
        else:
            self.imm16s(4, 'i32')
        f.op(op)
        f.op('i64.extend_i32_s')
        f.op('i64.store', 0)
        self.next()

    def link(self, rd_push, length):
        """REG(rd) = pc + length, rd_push pushes the address of REG(rd)"""
        f = self.f
        rd_push()
        self.pc_of_d()
        f.op('i64.const', length)
        f.op('i64.add')
        f.op('i64.store', 0)

    def upper_imm(self):
        """Push sext32(dword @4 << 8) (FasterJtype: offset24 @4..6, rd @7)"""
        f = self.f
        f.get('d')
        f.op('i32.load', 4)
        f.op('i32.const', 8)
        f.op('i32.shl')
        f.op('i64.extend_i32_s')

    def jump_by(self, push_offset):
        """$d += byte offset * BRSCALE, check, dispatch the block"""
        f = self.f
        f.get('d')
        push_offset()
        f.op('i32.const', self.PC2DEC)
        f.op('i32.shl')
        f.op('i32.add')
        f.set('d')
        self.check_d()
        self.dispatch_block()

    # -- The handlers --
    def handler(self, name):
        f = self.f
        self.cur_c = name.startswith('c_')
        H = self
        if name in ('fallback', 'default'):
            self.call_cpp(KIND_INVALID)
        elif name == 'stop':
            self.pc_of_d()
            f.op('i64.const', 4)
            f.op('i64.add')
            f.set('pc')
            f.op('i32.const', EXIT_STOP)
            f.set('code')
            f.br('exit_pc')
        # Immediate ALU, FasterItype
        elif name == 'addi':
            self.alu_imm('i64.add')
        elif name == 'li':
            # FasterImmediate: rd @4, imm16 @6
            self.reg_addr(4)
            self.imm16s(6)
            f.op('i64.store', 0)
            self.next()
        elif name in ('mv', 'c_mv'):
            # FasterMove: rs1 @4, rd @5
            self.reg_addr(5)
            self.reg(4)
            f.op('i64.store', 0)
            self.next()
        elif name == 'slli':
            self.alu_imm('i64.shl', 'u')
        elif name == 'srli':
            self.alu_imm('i64.shr_u', 'u')
        elif name == 'srai':
            self.alu_imm('i64.shr_s', 'u')
        elif name in ('slti', 'sltiu'):
            self.reg_addr(7)
            self.reg(6)
            self.imm16s()
            f.op('i64.lt_s' if name == 'slti' else 'i64.lt_u')
            f.op('i64.extend_i32_u')
            f.op('i64.store', 0)
            self.next()
        elif name == 'xori':
            self.alu_imm('i64.xor')
        elif name == 'ori':
            self.alu_imm('i64.or')
        elif name == 'andi':
            self.alu_imm('i64.and')
        elif name in ('sext_b', 'sext_h'):
            self.reg_addr(7)
            self.reg(6)
            f.op('i64.extend8_s' if name == 'sext_b' else 'i64.extend16_s')
            f.op('i64.store', 0)
            self.next()
        elif name == 'bseti':
            self.reg_addr(7)
            self.reg(6)
            f.op('i64.const', 1)
            self.imm16u()
            f.op('i64.shl')
            f.op('i64.or')
            f.op('i64.store', 0)
            self.next()
        elif name == 'bexti':
            self.reg_addr(7)
            self.reg(6)
            self.imm16u()
            f.op('i64.shr_u')
            f.op('i64.const', 1)
            f.op('i64.and')
            f.op('i64.store', 0)
            self.next()
        elif name in ('lui', 'c_lui'):
            self.reg_addr(7)
            self.upper_imm()
            f.op('i64.store', 0)
            self.next()
        elif name == 'auipc':
            self.reg_addr(7)
            self.upper_imm()
            self.pc_of_d()
            f.op('i64.add')
            f.op('i64.store', 0)
            self.next()
        # Register ALU, FasterOpType: rd16 @4, rs2 @6, rs1 @7
        elif name in ('op_add', 'op_sub', 'op_sll', 'op_srl', 'op_sra',
                      'op_xor', 'op_or', 'op_and', 'op_mul'):
            self.alu_reg({'op_add': 'i64.add', 'op_sub': 'i64.sub', 'op_sll': 'i64.shl',
                          'op_srl': 'i64.shr_u', 'op_sra': 'i64.shr_s',
                          'op_xor': 'i64.xor', 'op_or': 'i64.or', 'op_and': 'i64.and',
                          'op_mul': 'i64.mul'}[name])
        elif name in ('op_slt', 'op_sltu'):
            self.reg_addr(4, 'u16')
            self.reg(7)
            self.reg(6)
            f.op('i64.lt_s' if name == 'op_slt' else 'i64.lt_u')
            f.op('i64.extend_i32_u')
            f.op('i64.store', 0)
            self.next()
        elif name in ('op_div', 'op_divu', 'op_rem', 'op_remu'):
            self.division(name[3:])
        elif name == 'op_zext_h':
            self.reg_addr(4, 'u16')
            self.reg_addr(7)
            f.op('i64.load16_u', 0)
            f.op('i64.store', 0)
            self.next()
        elif name in ('op_sh1add', 'op_sh2add', 'op_sh3add'):
            self.shadd(int(name[5]))
        # RV64 32-bit operations, sign-extended to 64 bits
        elif name == 'addiw':
            self.reg_addr(7)
            self.reg32(6)
            self.imm16s(4, 'i32')
            f.op('i32.add')
            f.op('i64.extend_i32_s')
            f.op('i64.store', 0)
            self.next()
        elif name in ('slliw', 'srliw', 'sraiw'):
            self.reg_addr(7)
            self.reg32(6)
            self.imm16u(4, 'i32')
            f.op({'slliw': 'i32.shl', 'srliw': 'i32.shr_u', 'sraiw': 'i32.shr_s'}[name])
            f.op('i64.extend_i32_s')
            f.op('i64.store', 0)
            self.next()
        elif name in ('op_addw', 'op_subw', 'op_mulw'):
            self.alu_reg32({'op_addw': 'i32.add', 'op_subw': 'i32.sub', 'op_mulw': 'i32.mul'}[name])
        elif name == 'op_add_uw':
            self.shadd(0, uw=True)
        elif name in ('op_sh1add_uw', 'op_sh2add_uw'):
            self.shadd(int(name[5]), uw=True)
        # Calls into C++
        elif name in ('function', 'c_function'):
            self.function(KIND_C_FUNCTION if self.cur_c else KIND_FUNCTION)
        elif name == 'funcblock':
            self.call_cpp(KIND_FUNCBLOCK)
        elif name == 'syscall':
            self.call_cpp(KIND_SYSCALL)
        elif name == 'dyncall':
            self.call_cpp(KIND_DYNCALL)
        elif name == 'system':
            self.call_cpp(KIND_SYSTEM)
        elif name == 'translator':
            self.call_cpp(KIND_TRANSLATOR)
        elif name == 'asmjit':
            self.call_cpp(KIND_ASMJIT)
        elif name == 'livepatch':
            self.call_cpp(KIND_LIVEPATCH)
        elif name == 'step':
            self.call_cpp(KIND_STEP)
        elif name == 'c_step':
            self.call_cpp(KIND_C_STEP)
        # Branches. BEQ and BNE may go backwards: the rewriter turns the
        # forward ones into _FW, which do not need the counter check.
        elif name in ('beq', 'bne', 'beq_fw', 'bne_fw', 'blt', 'bge', 'bltu', 'bgeu'):
            cmp = {'beq': 'i64.eq', 'bne': 'i64.ne', 'blt': 'i64.lt_s',
                   'bge': 'i64.ge_s', 'bltu': 'i64.lt_u', 'bgeu': 'i64.ge_u'}[name.replace('_fw', '')]
            self.branch(cmp, check=not name.endswith('_fw'))
        # Jumps
        elif name == 'jal':
            # FasterJtype: rd = pc + 4, pc += sext(offset24)
            self.link(lambda: self.reg_addr(7), 4)
            def off():
                f.get('d')
                f.op('i32.load', 4)
                f.op('i32.const', 8)
                f.op('i32.shl')
                f.op('i32.const', 8)
                f.op('i32.shr_s')
            self.jump_by(off)
        elif name in ('fast_jal', 'fast_call'):
            if name == 'fast_call':
                self.link(lambda: (f.get('regs'), f.op('i32.const', 8), f.op('i32.add')), 4)
            self.jump_by(lambda: (f.get('d'), f.op('i32.load', 4)))
        elif name == 'jalr':
            # FasterItype: target = REG(rs2) + imm, rs1 = link register (0 = none)
            self.reg(6)
            self.imm16s()
            f.op('i64.add')
            f.op('i64.const', -2)
            f.op('i64.and')
            f.tee('pc')
            if not self.compressed:
                f.op('i32.wrap_i64')
                f.op('i32.const', 3)
                f.op('i32.and')
                self.fallback_if()      # C++ raises the misaligned exception
            else:
                f.op('drop')
            self.u8(7)
            f.op('if')
            f.stack.append(None)
            self.link(lambda: self.reg_addr(7), 4)
            f.end('if')
            self.jump_to_pc()
        # Loads and stores
        elif name in ('ldw', 'c_ldw'):
            self.load('i64.load32_s')
        elif name == 'ldh':
            self.load('i64.load16_s')
        elif name == 'ldhu':
            self.load('i64.load16_u')
        elif name == 'ldb':
            self.load('i64.load8_s')
        elif name == 'ldbu':
            self.load('i64.load8_u')
        elif name == 'ldwu':
            self.load('i64.load32_u')
        elif name in ('ldd', 'c_ldd'):
            self.load('i64.load')
        elif name in ('stw', 'c_stw'):
            self.store('i64.store32')
        elif name == 'sth':
            self.store('i64.store16')
        elif name == 'stb':
            self.store('i64.store8')
        elif name in ('std', 'c_std'):
            self.store('i64.store')
        elif name == 'flw':
            self.fp_load(32)
        elif name in ('fld', 'c_fld'):
            self.fp_load(64)
        elif name == 'fsw':
            self.fp_store(32)
        elif name in ('fsd', 'c_fsd'):
            self.fp_store(64)
        # Floating point arithmetic
        elif name in ('fadd', 'fsub', 'fmul', 'fdiv'):
            self.fp_arith(name[1:])
        elif name == 'fmadd':
            self.fmadd()
        # Compressed
        elif name == 'c_addi':
            self.alu_imm('i64.add')
        elif name == 'c_slli':
            self.creg_imm('i64.shl', 'u')
        elif name == 'c_srli':
            self.creg_imm('i64.shr_u', 'u')
        elif name == 'c_srai':
            self.creg_imm('i64.shr_s', 'u')
        elif name == 'c_andi':
            self.creg_imm('i64.and', 's')
        elif name in ('c_add', 'c_sub', 'c_and', 'c_xor', 'c_or'):
            self.creg_op('i64.' + name[2:])
        elif name in ('c_addw', 'c_subw'):
            self.creg_op32('i32.' + name[2:5], 'reg')
        elif name == 'c_addiw':
            self.creg_op32('i32.add', 'imm')
        elif name in ('c_bnez', 'c_beqz'):
            # Taken compressed branches check the counter in either direction,
            # like PERFORM_BRANCH in cpu_dispatch.cpp
            self.reg(7)
            if name == 'c_beqz':
                f.op('i64.eqz')
            else:
                f.op('i64.const', 0)
                f.op('i64.ne')
            f.if_()
            self.take_branch(True)
            f.end('if')
            self.next_block()
        elif name == 'c_jmp':
            self.take_branch(True)
        elif name in ('c_jr', 'c_jalr'):
            # instr = register index
            f.get('d')
            f.op('i32.load', 4)
            f.op('i32.const', 3)
            f.op('i32.shl')
            f.get('regs')
            f.op('i32.add')
            f.op('i64.load', 0)
            f.op('i64.const', -2)
            f.op('i64.and')
            f.set('pc')
            if name == 'c_jalr':
                self.link(lambda: (f.get('regs'), f.op('i32.const', 8), f.op('i32.add')), 2)
            self.jump_to_pc()
        else:
            raise ValueError(f'no handler: {name}')

    def function(self, kind):
        """FUNCTION: an instruction without a bytecode, executed by its handler
        in C++, like CPU::execute(). The handler is called directly rather than
        through asm_call(): FP comparisons, conversions and the like are common
        enough (STREAM: 30M), and asm_call() has a try/catch, which with
        Emscripten's JavaScript exceptions is a trip through JavaScript.

        An exception thrown by the handler unwinds through the dispatch, so the
        machine has to be up to date before the call: the PC, and (COUNTING)
        the instruction counter. Nothing else is kept in locals that C++ could
        need. Like the C++ dispatch, the segment may have become stale."""
        f = self.f
        length = 2 if kind == KIND_C_FUNCTION else 4
        self.u8(1)                      # m_handler: 0 means not decoded yet
        f.op('i32.eqz')
        f.if_()
        self.call_cpp(kind)
        f.end('if')
        f.get('regs')
        self.pc_of_d()
        f.op('i64.store', PCOFF)
        if self.counting:
            self.ctx_load('counter_ptr', 'i32')
            f.get('counter')
            f.op('i64.store', 0)
        # The handler takes rv32i_instruction (a union) by value, which the
        # wasm32 C ABI passes by reference
        f.get('ctx')
        f.get('d')
        f.op('i32.load', 4)
        f.op('i32.store', CTX['instr'])
        self.ctx_load('cpu', 'i32')
        f.get('ctx')
        f.op('i32.const', CTX['instr'])
        f.op('i32.add')
        self.ctx_load('handlers', 'i32')
        self.u8(1)
        f.op('i32.const', 2)
        f.op('i32.shl')
        f.op('i32.add')
        f.op('i32.load', 0)
        f.op('call_indirect', '__indirect_function_table,', '(i32, i32) -> ()')
        self.ctx_load('stale', 'i32')
        f.op('i32.load8_u', 0)
        f.if_()
        self.pc_of_d()
        f.op('i64.const', length)
        f.op('i64.add')
        f.set('pc')
        f.op('i32.const', EXIT_JUMP)
        f.set('code')
        f.br('exit_pc')
        f.end('if')
        self.next()

    def fmadd(self):
        """The original instruction (R4type): rd @7, rs1 @15, rs2 @20, funct2 @25,
        rs3 @27. rd = rs1 * rs2 + rs3, rounded once (fma/fmaf in libc)."""
        f = self.f
        f.get('d')
        f.op('i32.load', 4)
        f.tee('t')
        f.op('i32.const', 1 << 26)      # funct2 >= 2 (half, quad)
        f.op('i32.and')
        self.fallback_if()

        def fregp(shift):
            f.get('t')
            f.op('i32.const', shift)
            f.op('i32.shr_u')
            f.op('i32.const', 31)
            f.op('i32.and')
            f.op('i32.const', 3)
            f.op('i32.shl')
            f.get('regs')
            f.op('i32.add')
        f.get('t')
        f.op('i32.const', 1 << 25)
        f.op('i32.and')
        f.if_()
        for t, fn in (('f64', 'fma'), ('f32', 'fmaf')):
            fregp(7)
            for s in (15, 20, 27):
                fregp(s)
                f.op(f'{t}.load', FPOFF)
            f.op('call', fn)
            f.op(f'{t}.store', FPOFF)
            self.next()
            if t == 'f64':
                f.else_()
        f.end('if')
        f.op('unreachable')

    # -- The whole function --
    def build(self):
        f = self.f
        names = []
        for bc in self.bytecodes:
            h = self.hmap.get(bc, 'fallback')
            if h not in names:
                names.append(h)
        if 'fallback' not in names:
            names.insert(0, 'fallback')
        # The default of the br_table (bytecodes that do not exist) is a copy of
        # the fallback, so that bytecode 0 (INVALID, the fallback) is not the
        # default too: wasm-opt then subtracts 1 from every bytecode to shorten
        # the table, which costs an instruction per dispatch.
        names.append('default')
        # Entry
        self.ctx_load('regs', 'i32'); f.set('regs')
        self.ctx_load('decoder', 'i32'); f.set('dec')
        self.ctx_load('arena', 'i32'); f.set('arena')
        self.ctx_load('read_boundary'); f.set('rbound')
        self.ctx_load('rodata_end'); f.set('rodata')
        self.ctx_load('write_boundary'); f.set('wbound')
        self.ctx_load('begin'); f.set('begin')
        self.ctx_load('length'); f.set('length')
        self.ctx_load('budget', 'i32'); f.set('budget')
        if self.counting:
            self.ctx_load('counter'); f.set('counter')
            self.ctx_load('max_counter'); f.set('max')
        f.get('dec')
        self.ctx_load('pc')
        f.op('i32.wrap_i64')
        f.op('i32.const', self.PC2DEC)
        f.op('i32.shl')
        f.op('i32.add')
        f.set('d')
        if self.counting:
            f.get('ctx')
            f.op('i32.load8_u', CTX['enter_count'])
            f.op('if')
            f.stack.append(None)
            self.count_block()
            f.end('if')

        f.block('ret')
        f.loop('outer')
        f.get('d')
        f.op('i32.load8_u', 0)
        f.set('bc')
        f.block('goto')
        f.block('call')
        f.block('exit_pc')
        f.loop('L')
        for n in reversed(names):
            f.block('h_' + n)
        f.comment('dispatch')
        f.get('bc')
        targets = ['h_' + self.hmap.get(bc, 'fallback') for bc in self.bytecodes]
        f.br_table(targets + ['h_default'])
        for n in names:
            f.end('block')
            f.comment(f'handler {n}')
            self.handler(n)
        f.end('loop')
        f.end('block')
        # exit_pc: ctx->pc = $pc, return $code
        self.ctx_store_local('pc', 'pc')
        f.br('ret')
        f.end('block')
        # call: execute the current entry in C++ ($kind)
        self.do_call()
        f.end('block')
        # goto: continue at $pc
        self.jump_to_pc_outer()
        f.end('loop')
        f.end('block')
        # ret
        if self.counting:
            self.ctx_store_local('counter', 'counter')
            self.ctx_store_local('max_counter', 'max')
        f.get('code')
        return f

    def do_call(self):
        f = self.f
        if self.counting:
            self.ctx_store_local('counter', 'counter')
            self.ctx_store_local('max_counter', 'max')
        # Through the pointer in the context, rather than a direct call:
        # wasm-opt inlines functions that have one caller, and all of
        # rv64wasm_call() inside the dispatch made the hot loop slower.
        f.get('ctx')
        f.get('d')
        f.get('kind')
        self.ctx_load('call', 'i32')
        f.op('call_indirect', '__indirect_function_table,', '(i32, i32, i32) -> (i32)')
        f.set('code')
        # The arena boundaries can change in any system call
        self.ctx_load('read_boundary'); f.set('rbound')
        self.ctx_load('rodata_end'); f.set('rodata')
        self.ctx_load('write_boundary'); f.set('wbound')
        self.ctx_load('arena', 'i32'); f.set('arena')
        if self.counting:
            # So can the counters (penalties, stopping)
            self.ctx_load('counter'); f.set('counter')
            self.ctx_load('max_counter'); f.set('max')
        f.get('code')
        f.op('i32.const', CALL_NEXT)
        f.op('i32.ne')
        f.if_()
        # CALL_GOTO: continue at ctx->pc, otherwise leave with the exit code
        # (ctx->pc written by C++)
        self.ctx_load('pc')
        f.set('pc')
        f.get('code')
        f.op('i32.const', CALL_GOTO)
        f.op('i32.eq')
        f.br_if('goto')
        f.br('ret')
        f.end('if')
        # CALL_NEXT: the next instruction. After a system call (or SYSTEM)
        # it begins a block.
        f.get('kind')
        f.op('i32.const', KIND_C_STEP)
        f.op('i32.eq')
        f.get('kind')
        f.op('i32.const', KIND_C_FUNCTION)
        f.op('i32.eq')
        f.op('i32.or')
        f.if_()
        self.advance(self.I2)
        f.br('outer')
        f.end('if')
        self.advance(self.I4)
        if self.counting:
            f.get('kind')
            f.op('i32.const', KIND_SYSCALL)
            f.op('i32.eq')
            f.get('kind')
            f.op('i32.const', KIND_SYSTEM)
            f.op('i32.eq')
            f.op('i32.or')
            f.if_()
            self.count_block()
            f.end('if')
        f.br('outer')

    def jump_to_pc_outer(self):
        """The goto block is outside of the dispatch loop: jump_to_pc() with
        exits that write ctx->pc themselves and continue through $outer."""
        f = self.f
        def leave(code):
            f.op('i32.const', code)
            f.set('code')
            self.ctx_store_local('pc', 'pc')
            f.br('ret')
        if self.counting:
            f.get('counter')
            f.get('max')
            f.op('i64.ge_u')
            f.if_()
            leave(EXIT_OVERFLOW)
            f.end('if')
        f.get('pc')
        f.get('begin')
        f.op('i64.sub')
        f.get('length')
        f.op('i64.ge_u')
        f.if_()
        leave(EXIT_JUMP)
        f.end('if')
        f.get('dec')
        f.get('pc')
        f.op('i32.wrap_i64')
        f.op('i32.const', self.PC2DEC)
        f.op('i32.shl')
        f.op('i32.add')
        f.set('d')
        self.count_block()
        f.br('outer')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument('--compressed', type=int, default=1)
    ap.add_argument('--translator', type=int, default=0)
    ap.add_argument('--asmjit', type=int, default=0)
    ap.add_argument('--nanboxing', type=int, default=0)
    ap.add_argument('--fcsr', type=int, default=0)
    ap.add_argument('-o', '--output', required=True, help='the .s file')
    ap.add_argument('--header', required=True, help='the C++ header with the layout checks')
    args = ap.parse_args()

    bytecodes = bytecode_list(args.compressed, args.translator, args.asmjit)
    hmap = handler_map(args.compressed, args.nanboxing, args.fcsr)

    out = ['# Generated by dispatch_rv64.py. Do not edit.',
           '\t.functype\tfma (f64, f64, f64) -> (f64)',
           '\t.functype\tfmaf (f32, f32, f32) -> (f32)',
           '\t.tabletype\t__indirect_function_table, funcref']
    for counting in (False, True):
        d = Dispatch(counting, args.compressed, bytecodes, hmap)
        d.build().emit(out)
    with open(args.output, 'w') as fp:
        fp.write('\n'.join(out) + '\n')

    hdr = ['// Generated by dispatch_rv64.py. Do not edit.',
           '#pragma once',
           f'#define RV64WASM_COMPRESSED {args.compressed}',
           f'#define RV64WASM_TRANSLATOR {args.translator}',
           f'#define RV64WASM_ASMJIT {args.asmjit}',
           f'#define RV64WASM_NANBOXING {args.nanboxing}',
           f'#define RV64WASM_FCSR {args.fcsr}',
           f'#define RV64WASM_FPOFF {FPOFF}',
           f'#define RV64WASM_PCOFF {PCOFF}',
           f'#define RV64WASM_BYTECODES_MAX {len(bytecodes)}',
           '#define RV64WASM_CHECK_CONTEXT(T) \\']
    for k, v in CTX.items():
        hdr.append(f'\tstatic_assert(offsetof(T, {k}) == {v}, "wasm dispatch: context layout"); \\')
    hdr.append('\t/* */')
    hdr.append('#define RV64WASM_CHECK_BYTECODES() \\')
    for i, bc in enumerate(bytecodes):
        hdr.append(f'\tstatic_assert({bc} == {i}, "wasm dispatch: bytecode numbering"); \\')
    hdr.append(f'\tstatic_assert(BYTECODES_MAX == {len(bytecodes)}, "wasm dispatch: bytecode count");')
    with open(args.header, 'w') as fp:
        fp.write('\n'.join(hdr) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
