#!/usr/bin/env python3
"""Reference disassembly via GNU objdump, normalized into `addr<TAB>text` lines.

The normalized form is the contract that libriscv's instruction printers must
reproduce byte-for-byte:

  * `objdump -d -M no-aliases` (canonical mnemonics, ABI register names)
  * symbols removed, so branch/jump targets always render as `0x<lowercase hex>`
  * objdump's derived `# ...` trailing comments removed
  * exactly one TAB between the mnemonic and its operands
"""
import os, re, shutil, subprocess, sys, tempfile

DEFAULT_ARCH = "rv64gc_zba_zbb_zbc_zbs_zicond_zicbom_zicboz_zifencei_zfa_zbkb"

def _tool(name):
    for prefix in ("riscv64-unknown-elf-", "riscv64-linux-gnu-"):
        p = shutil.which(prefix + name)
        if p:
            return p
    raise SystemExit("could not find a riscv64 " + name)

AS = _tool("as")
OBJDUMP = _tool("objdump")
STRIP = _tool("strip")

LINE = re.compile(r"^\s*([0-9a-f]+):\t([0-9a-f ]+)\t(.*)$")

def assemble(words, arch=DEFAULT_ARCH):
    """Assemble (length, value) instruction words into a stripped ELF object.

    Uses `.insn` rather than `.word` so the assembler marks the bytes as code;
    `.word` emits `$d` mapping symbols and objdump then renders them as data.
    """
    src = ['.attribute arch, "%s"' % arch, ".text"]
    for length, value in words:
        src.append("\t.insn %d, 0x%x" % (length, value))
    tmp = tempfile.mkdtemp(prefix="refdis")
    s, o = os.path.join(tmp, "c.s"), os.path.join(tmp, "c.o")
    with open(s, "w") as f:
        f.write("\n".join(src) + "\n")
    subprocess.run([AS, "-o", o, s], check=True)
    subprocess.run([STRIP, o], check=True)
    return o

def disassemble(path, arch=None):
    """Return an ordered list of (address, normalized text) for an ELF file."""
    cmd = [OBJDUMP, "-d", "-M", "no-aliases", path]
    if arch:
        cmd[3:3] = ["-m", "riscv"]
    out = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout
    result = []
    for line in out.splitlines():
        m = LINE.match(line)
        if not m:
            continue
        result.append((int(m.group(1), 16), normalize(m.group(3))))
    return result

def normalize(text):
    text = text.split("\t#")[0].split("   #")[0]
    text = re.sub(r"\s+#.*$", "", text)
    text = re.sub(r"\s*<[^>]*>", "", text)
    parts = text.split(None, 1)
    if len(parts) == 1:
        return parts[0]
    return parts[0] + "\t" + parts[1].strip()


def elf_sections(path):
    """Executable sections of an ELF, as (name, vma, offset, size)."""
    out = subprocess.run([_tool("readelf"), "-S", "-W", path],
                         check=True, capture_output=True, text=True).stdout
    sections = []
    for line in out.splitlines():
        m = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+([0-9a-f]+)\s+"
                     r"([0-9a-f]+)\s+([0-9a-f]+)\s+\S+\s+(\S*)", line)
        if not m:
            continue
        name, kind, addr, off, size, flags = m.groups()
        if kind == "PROGBITS" and "X" in flags and int(size, 16) > 0:
            sections.append((name, int(addr, 16), int(off, 16), int(size, 16)))
    return sections


def elf_xlen(path):
    out = subprocess.run([_tool("readelf"), "-h", path],
                         check=True, capture_output=True, text=True).stdout
    return 32 if "ELF32" in out else 64


def strip_copy(path):
    """A stripped copy of an ELF, so objdump renders targets uniformly as 0x...

    With a symbol table objdump writes `beq a0,a1,10154 <main+0x8>`; without one
    it writes `beq a0,a1,0x10154`, which is the form the printers produce.
    """
    tmp = tempfile.mkdtemp(prefix="refdis")
    out = os.path.join(tmp, os.path.basename(path))
    subprocess.run([_tool("objcopy"), "--strip-all", path, out], check=True)
    return out


if __name__ == "__main__":
    for addr, text in disassemble(sys.argv[1]):
        print("%x\t%s" % (addr, text))
