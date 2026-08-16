#!/usr/bin/env python3
"""Instruction corpora for the objdump differential test.

These sweep the encoding space rather than sampling it. The compressed corpus is
literally every valid 16-bit encoding; the 32-bit ones are exhaustive over the
bits that select an instruction (funct3/funct7/funct5, and the full 12-bit
immediate wherever the immediate is what distinguishes one instruction from
another), crossed with a handful of register patterns.

The register patterns matter as much as the opcode bits: libriscv's decoder
takes shortcuts on `rd == 0` and on zero immediates, routing those encodings to
different handlers than the general case, so the corpus has to exercise them.
"""

# rd, rs1, rs2. The first is the general case with three distinguishable
# registers; the rest hit the x0 special cases the decoder branches on.
REG_PATTERNS = [
    (15, 11, 12),   # a5, a1, a2
    (0,  11, 12),   # rd = zero
    (15,  0, 12),   # rs1 = zero
    (15, 11,  0),   # rs2 = zero
    (0,   0,  0),   # all zero
    (2,   2,  2),   # same register three times
]

# Immediates worth trying when we are not sweeping the field exhaustively:
# zero, small, all-ones, both sides of the sign boundary, an alternating pattern.
IMM_SAMPLES = [0, 1, 0x7FF, 0x800, 0xFFF, 0x555, 0xAAA, 0x20, 0x3F, 0x40]

RV64GC = "rv64gcv_zcb_zba_zbb_zbc_zbs_zicond_zicbom_zicboz_zifencei_zfa_zbkb_zvbb_zvkb"
RV32GC = "rv32gcv_zcb_zba_zbb_zbc_zbs_zicond_zicbom_zicboz_zifencei_zfa_zbkb_zvbb_zvkb"


def r_type(opcode, rd, funct3, rs1, rs2, funct7):
    return (opcode | (rd << 7) | (funct3 << 12) | (rs1 << 15)
            | (rs2 << 20) | (funct7 << 25)) & 0xFFFFFFFF


def i_type(opcode, rd, funct3, rs1, imm):
    return (opcode | (rd << 7) | (funct3 << 12) | (rs1 << 15)
            | ((imm & 0xFFF) << 20)) & 0xFFFFFFFF


def s_type(opcode, funct3, rs1, rs2, imm):
    imm = imm & 0xFFF
    return (opcode | ((imm & 0x1F) << 7) | (funct3 << 12) | (rs1 << 15)
            | (rs2 << 20) | ((imm >> 5) << 25)) & 0xFFFFFFFF


def r4_type(opcode, rd, rm, rs1, rs2, fmt, rs3):
    return (opcode | (rd << 7) | (rm << 12) | (rs1 << 15) | (rs2 << 20)
            | (fmt << 25) | (rs3 << 27)) & 0xFFFFFFFF


class Corpus:
    """A named set of instruction words, plus the ISA to interpret them under."""

    def __init__(self, name, fn, arch=RV64GC, xlen=64):
        self.name, self.fn, self.arch, self.xlen = name, fn, arch, xlen

    def build(self):
        words = self.fn()
        # `.insn N, x` refuses a word whose low bits contradict N, and those
        # encodings are 48-bit or longer anyway -- out of scope for both sides.
        return [(n, w) for (n, w) in words
                if (n == 4 and (w & 3) == 3 and (w & 0x1C) != 0x1C)
                or (n == 2 and (w & 3) != 3)]


def compressed():
    """Every valid 16-bit encoding, all 49152 of them."""
    return [(2, w) for w in range(0x10000) if (w & 3) != 3]


def _sweep_reg_opcodes(opcodes):
    """funct7 x funct3 x register patterns, for the register-register opcodes."""
    out = []
    for opcode in opcodes:
        for funct7 in range(128):
            for funct3 in range(8):
                for rd, rs1, rs2 in REG_PATTERNS:
                    out.append((4, r_type(opcode, rd, funct3, rs1, rs2, funct7)))
    return out


def _sweep_imm_opcode(opcode, exhaustive=True):
    """funct3 x the full 12-bit immediate, plus samples on the other reg patterns."""
    out = []
    rd, rs1, _ = REG_PATTERNS[0]
    for funct3 in range(8):
        if exhaustive:
            for imm in range(4096):
                out.append((4, i_type(opcode, rd, funct3, rs1, imm)))
        for rd2, rs12, _ in REG_PATTERNS[1:]:
            for imm in IMM_SAMPLES:
                out.append((4, i_type(opcode, rd2, funct3, rs12, imm)))
    return out


def integer():
    """OP and OP-32: add/sub/shift/compare, M, Zba, Zbb, Zbc, Zbs, Zicond."""
    return _sweep_reg_opcodes([0x33, 0x3B])


def integer_imm():
    """OP-IMM and OP-IMM-32, exhaustive over the immediate.

    Sweeping the whole immediate is what covers the Zbb unary instructions
    (clz/ctz/cpop/sext.b/sext.h/rev8/orc.b), whose selector lives in the
    immediate field, along with every shift amount and shift-op encoding.
    """
    return _sweep_imm_opcode(0x13) + _sweep_imm_opcode(0x1B)


def loads():
    """LOAD, JALR: funct3 x the full offset."""
    return _sweep_imm_opcode(0x03) + _sweep_imm_opcode(0x67)


def stores():
    out = []
    for funct3 in range(8):
        for imm in range(4096):
            out.append((4, s_type(0x23, funct3, 11, 12, imm)))
        for _, rs1, rs2 in REG_PATTERNS[1:]:
            for imm in IMM_SAMPLES:
                out.append((4, s_type(0x23, funct3, rs1, rs2, imm)))
    return out


def branches():
    """BRANCH: funct3 x the full 13-bit branch displacement."""
    out = []
    for funct3 in range(8):
        for imm in range(4096):
            out.append((4, s_type(0x63, funct3, 11, 12, imm)))
    return out


def upper_imm():
    """LUI, AUIPC, JAL: sweep the 20-bit field in strides, plus edge values."""
    out = []
    values = list(range(0, 1 << 20, 977)) + [0, 1, 0xFFFFF, 0x80000, 0x7FFFF]
    for imm in values:
        for rd in (15, 0, 1):
            out.append((4, (0x37 | (rd << 7) | (imm << 12)) & 0xFFFFFFFF))
            out.append((4, (0x17 | (rd << 7) | (imm << 12)) & 0xFFFFFFFF))
            out.append((4, (0x6F | (rd << 7) | (imm << 12)) & 0xFFFFFFFF))
    return out


def system():
    """SYSTEM: every CSR against every CSR opcode, plus ecall/ebreak/mret/wfi."""
    out = []
    for funct3 in range(8):
        for csr in range(4096):
            out.append((4, i_type(0x73, 15, funct3, 11, csr)))
        for rd, rs1, _ in REG_PATTERNS[1:]:
            for csr in (0, 1, 3, 0x300, 0xC00, 0xFFF):
                out.append((4, i_type(0x73, rd, funct3, rs1, csr)))
    return out


def misc_mem():
    """MISC-MEM: fence, fence.tso, fence.i, and the Zicbo hints."""
    return _sweep_imm_opcode(0x0F)


def atomics():
    """AMO: funct5 x aq/rl x width x register patterns."""
    out = []
    for funct5 in range(32):
        for aqrl in range(4):
            for funct3 in range(8):
                for rd, rs1, rs2 in REG_PATTERNS:
                    funct7 = (funct5 << 2) | aqrl
                    out.append((4, r_type(0x2F, rd, funct3, rs1, rs2, funct7)))
    return out


def float_ops():
    """OP-FP: funct7 x rounding mode x rs2, which selects the conversions."""
    out = []
    for funct7 in range(128):
        for rm in range(8):
            for rs2 in range(32):
                out.append((4, r_type(0x53, 15, rm, 11, rs2, funct7)))
    for funct7 in range(128):
        for rd, rs1, rs2 in REG_PATTERNS[1:]:
            out.append((4, r_type(0x53, rd, 0, rs1, rs2, funct7)))
    return out


def float_fused():
    """FMADD/FMSUB/FNMSUB/FNMADD: format x rounding mode x rs3."""
    out = []
    for opcode in (0x43, 0x47, 0x4B, 0x4F):
        for fmt in range(4):
            for rm in range(8):
                for rs3 in range(32):
                    out.append((4, r4_type(opcode, 15, rm, 11, 12, fmt, rs3)))
    return out


def float_mem():
    """FP LOAD and STORE: funct3 selects the width, sweep the full offset."""
    out = _sweep_imm_opcode(0x07)
    for funct3 in range(8):
        for imm in range(4096):
            out.append((4, s_type(0x27, funct3, 11, 12, imm)))
    return out


def v_type(opcode, vd, funct3, src, vs2, vm, funct6):
    return (opcode | (vd << 7) | (funct3 << 12) | (src << 15) | (vs2 << 20)
            | (vm << 25) | (funct6 << 26)) & 0xFFFFFFFF


def vector_op():
    """OP-V: funct3 x funct6 x vm, sweeping the field that feeds the operation.

    That field is vs1, rs1 or a 5-bit immediate depending on funct3, and a
    dozen code points read it as an opcode extension instead (vzext, the mask
    scans, the conversions), so it has to be swept rather than sampled. vs2
    likewise: the moves that write a whole register -- vmv.v.v and friends --
    exist only where vs2 is zero, and are otherwise reserved.
    """
    out = []
    for funct3 in range(8):
        if funct3 == 7:   # vector configuration, its own corpus below
            continue
        for funct6 in range(64):
            for vm in (0, 1):
                for src in range(32):
                    for vs2 in (12, 0):
                        out.append((4, v_type(0x57, 8, funct3, src, vs2, vm, funct6)))
    return out


def vector_config():
    """vsetvli, vsetivli and vsetvl, exhaustive over the vtype immediate.

    binutils spells out a vtype it recognises (`e8,m1,ta,ma`) and prints the
    raw number for one it does not, so the whole immediate has to be swept to
    pin down where the boundary sits.
    """
    out = []
    for zimm in range(1 << 11):      # vsetvli: 11-bit vtype
        for rd, rs1, _ in REG_PATTERNS[:3]:
            out.append((4, v_type(0x57, rd, 7, rs1, 0, 0, 0) | (zimm << 20)))
    for zimm in range(1 << 10):      # vsetivli: 10-bit vtype, 5-bit AVL
        out.append((4, 0xC0000057 | (8 << 7) | (7 << 12) | (5 << 15) | (zimm << 20)))
    for funct7 in range(128):        # vsetvl: only funct7 0x40 is the real one
        for rd, rs1, rs2 in REG_PATTERNS:
            out.append((4, r_type(0x57, rd, 7, rs1, rs2, funct7)))
    return out


def vector_mem():
    """Vector LOAD-FP and STORE-FP: addressing mode x segment count x width.

    The source field is the addressing sub-mode for unit-stride accesses and a
    register everywhere else, so it is swept whole; nf doubles as the register
    count of the whole-register forms.
    """
    out = []
    for opcode in (0x07, 0x27):
        for width in range(8):
            for mew in (0, 1):
                for mop in range(4):
                    for vm in (0, 1):
                        for field in range(32):
                            for nf in range(8):
                                out.append((4, (opcode | (8 << 7) | (width << 12)
                                    | (11 << 15) | (field << 20) | (vm << 25)
                                    | (mop << 26) | (mew << 28) | (nf << 29))
                                    & 0xFFFFFFFF))
    return out


CORPORA = {}


def _register(name, fn, arch=RV64GC, xlen=64):
    CORPORA[name] = Corpus(name, fn, arch, xlen)


_register("rvc", compressed)
_register("rvc32", compressed, arch=RV32GC, xlen=32)
_register("integer", integer)
_register("integer_imm", integer_imm)
_register("loads", loads)
_register("stores", stores)
_register("branches", branches)
_register("upper_imm", upper_imm)
_register("system", system)
_register("misc_mem", misc_mem)
_register("atomics", atomics)
_register("float_ops", float_ops)
_register("float_fused", float_fused)
_register("float_mem", float_mem)
_register("vector_op", vector_op)
_register("vector_config", vector_config)
_register("vector_mem", vector_mem)
_register("integer32", integer, arch=RV32GC, xlen=32)
_register("integer_imm32", integer_imm, arch=RV32GC, xlen=32)
_register("float_ops32", float_ops, arch=RV32GC, xlen=32)


if __name__ == "__main__":
    for name, c in CORPORA.items():
        print("%-16s %8d instructions  (%s)" % (name, len(c.build()), c.arch))
