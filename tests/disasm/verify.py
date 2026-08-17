#!/usr/bin/env python3
"""Prove that libriscv decodes RISC-V exactly the way binutils does.

The same instruction bytes are pushed through two disassemblers -- libriscv's
own decoder plus its instruction printers, and `objdump -d -M no-aliases` -- and
the two outputs must agree character for character. Any disagreement is either a
decoding bug (libriscv reached a different instruction than binutils did) or a
printer bug, and the run stops at the first one.

    ./verify.py                    # every corpus, stop at the first mismatch
    ./verify.py --corpus rvc       # just one corpus
    ./verify.py --all-mismatches   # keep going, summarise by mnemonic
    ./verify.py --elf a.out        # check a real program's .text instead

Corpora are generated in corpus.py; they sweep the encoding space rather than
sampling it, so `rvc` alone covers all 49152 valid 16-bit encodings.
"""
import argparse, os, re, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import corpus, refdis

HERE = os.path.dirname(os.path.abspath(__file__))
DISASM = os.path.join(HERE, "build", "disasm")


def run_libriscv(words, base, xlen):
    """Disassemble the corpus with libriscv, returning [(addr, text)]."""
    blob = bytearray()
    for length, value in words:
        blob += value.to_bytes(length, "little")
    path = os.path.join(HERE, "build", "corpus.bin")
    with open(path, "wb") as f:
        f.write(blob)

    cmd = [DISASM, "--rv%d" % xlen, "--base", hex(base), path]
    out = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout
    result = []
    for line in out.splitlines():
        addr, _, text = line.partition("\t")
        result.append((int(addr, 16), text))
    return result


MISSING = "<nothing decoded at this address>"

# Encodings where libriscv is deliberately stricter than binutils, keyed by
# (length, encoding) and recording exactly what each side says. The entry only
# suppresses the report while both sides keep saying precisely this, so any
# change on either side still fails the run.
#
# Keep this list short and justified; it is not a place to hide real mismatches.
KNOWN_DIVERGENCES = {
    (2, 0x6101): (
        "c.addi16sp\tsp,0",
        ".insn\t2, 0x6101",
        "C.ADDI16SP with nzimm=0 is a reserved code point (insn:c-addi16sp_rsv). "
        "libriscv rejects it; binutils decodes it anyway.",
    ),
}


# binutils decodes RV32 shift instructions with a six-bit shift amount, the RV64
# width, even for an ELF32 object. The encodings with shamt[5] set are reserved
# on RV32 (insn:c-slli_rsv and friends) and libriscv rejects them, which is what
# the ISA asks for -- so this whole class is expected, not a decoding bug.
SHIFT_MNEMONICS = ("slli", "srli", "srai", "rori", "bseti", "bclri", "binvi",
                   "bexti", "c.slli", "c.srli", "c.srai")


def is_rv32_reserved_shamt(xlen, want, got):
    if xlen != 32 or not got.startswith(".insn"):
        return False
    mnemonic, _, operands = want.partition("\t")
    if mnemonic not in SHIFT_MNEMONICS:
        return False
    shamt = operands.rsplit(",", 1)[-1]
    return shamt.startswith("0x") and int(shamt, 16) >= 0x20


# libriscv models a user-mode guest, so the supervisor-only instructions are
# not decoded at all -- objdump names them, libriscv rejects them, and that is
# the intended behaviour rather than a decoding bug. (SRET and MRET are the
# exceptions: they are printed, because a guest that executes one is worth
# naming in a trap message.)
PRIVILEGED_MNEMONICS = ("sfence.vma", "sfence.vm", "sinval.vma", "sfence.w.inval",
                        "sfence.inval.ir", "uret", "hret", "dret",
                        "hfence.vvma", "hfence.gvma")


def is_privileged_only(want, got):
    if not got.startswith(".insn"):
        return False
    return want.partition("\t")[0] in PRIVILEGED_MNEMONICS


def compare(name, words, arch, xlen, stop_first, limit):
    """Disassemble one corpus both ways and report every disagreement."""
    arch, dropped = refdis.supported_arch(arch)
    obj = refdis.assemble(words, arch=arch)
    expected = refdis.disassemble(obj)
    actual = run_libriscv(words, 0, xlen)

    # Address of each corpus word, so a mismatch can name the encoding.
    encodings, addr = {}, 0
    for length, value in words:
        encodings[addr] = (length, value)
        addr += length

    exp_by_addr = dict(expected)
    act_by_addr = dict(actual)

    mismatches, waived = [], 0
    for addr, want in expected:
        # A missing line means libriscv disagreed about some earlier
        # instruction's length and the two disassemblers fell out of sync.
        got = act_by_addr.get(addr, MISSING)
        if got == want:
            continue
        known = KNOWN_DIVERGENCES.get(encodings.get(addr))
        if known and (want, got) == known[:2]:
            waived += 1
            continue
        if is_rv32_reserved_shamt(xlen, want, got):
            waived += 1
            continue
        if is_privileged_only(want, got):
            waived += 1
            continue
        # This binutils predates part of the ISA the corpus asks for, so it
        # renders those encodings as raw words while libriscv names them.
        # Waive that one shape and only that one -- anywhere the reference
        # did decode the instruction, the comparison stays strict.
        if dropped and want.startswith(".insn") and not got.startswith(".insn"):
            waived += 1
            continue
        mismatches.append((addr, want, got))
        if stop_first:
            break
    if not (mismatches and stop_first):
        for addr, got in actual:
            if addr not in exp_by_addr:
                mismatches.append((addr, MISSING, got))
                if stop_first:
                    break

    print("%-14s %8d instructions, %6d mismatches%s%s"
          % (name, len(expected), len(mismatches),
             ", %d known divergences waived" % waived if waived else "",
             "  [binutils lacks %s]" % ",".join(dropped) if dropped else ""))
    for addr, want, got in mismatches[:limit]:
        length, value = encodings.get(addr, (4, 0))
        print("    %0*x  (at 0x%x)" % (length * 2, value, addr))
        print("        objdump : %s" % want.replace("\t", " | "))
        print("        libriscv: %s" % got.replace("\t", " | "))
    if len(mismatches) > limit:
        print("    ... and %d more" % (len(mismatches) - limit))
    return mismatches



def verify_elf(path, limit):
    """Disassemble a real program's executable sections both ways and diff."""
    xlen = refdis.elf_xlen(path)
    stripped = refdis.strip_copy(path)
    expected = dict(refdis.disassemble(stripped))

    with open(path, "rb") as f:
        image = f.read()

    total, mismatches = 0, []
    for name, vma, off, size in refdis.elf_sections(path):
        code = image[off:off + size]
        blob = os.path.join(HERE, "build", "section.bin")
        with open(blob, "wb") as f:
            f.write(code)
        cmd = [DISASM, "--rv%d" % xlen, "--base", hex(vma), blob]
        out = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout
        for line in out.splitlines():
            addr, _, got = line.partition("\t")
            addr = int(addr, 16)
            want = expected.get(addr)
            if want is None:
                # objdump skips data interleaved in code sections; so do we.
                continue
            total += 1
            if got != want:
                mismatches.append((addr, want, got))

    print("%-14s %8d instructions, %6d mismatches"
          % (os.path.basename(path), total, len(mismatches)))
    for addr, want, got in mismatches[:limit]:
        print("    at 0x%x" % addr)
        print("        objdump : %s" % want.replace("\t", " | "))
        print("        libriscv: %s" % got.replace("\t", " | "))
    if len(mismatches) > limit:
        print("    ... and %d more" % (len(mismatches) - limit))
    return mismatches


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", action="append", help="only run these corpora")
    ap.add_argument("--all-mismatches", action="store_true",
                    help="report every mismatch instead of stopping at the first")
    ap.add_argument("--limit", type=int, default=20, help="mismatches to print per corpus")
    ap.add_argument("--elf", action="append",
                    help="verify a real program's executable sections")
    ap.add_argument("--list", action="store_true", help="list available corpora")
    args = ap.parse_args()

    if args.list:
        for name in corpus.CORPORA:
            print(name)
        return 0

    if not os.path.exists(DISASM):
        print("build the tool first: cmake -B build . && cmake --build build", file=sys.stderr)
        return 2

    if args.elf:
        bad = sum(len(verify_elf(e, args.limit)) for e in args.elf)
        print("\n" + ("FAILED: %d mismatches" % bad if bad
                       else "OK: libriscv matches objdump on every instruction"))
        return 1 if bad else 0

    names = args.corpus or list(corpus.CORPORA)
    total = 0
    for name in names:
        if name not in corpus.CORPORA:
            print("unknown corpus %r; --list to see them all" % name, file=sys.stderr)
            return 2
        gen = corpus.CORPORA[name]
        words = gen.build()
        bad = compare(name, words, gen.arch, gen.xlen,
                      stop_first=not args.all_mismatches, limit=args.limit)
        total += len(bad)
        if bad and not args.all_mismatches:
            print("\nFAILED: first mismatch in corpus %r" % name)
            return 1

    if total:
        print("\nFAILED: %d mismatches" % total)
        return 1
    print("\nOK: libriscv matches objdump on every instruction")
    return 0


if __name__ == "__main__":
    sys.exit(main())
