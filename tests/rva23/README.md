# RVA23 differential tests

Every program in `progs/` is built twice — once for RVA23 with the vector
extension on, once natively — and the two runs must print the same thing.
A vector instruction that computes the wrong answer shows up as a diff; one
that is missing shows up as a trap.

```
./run.sh                 # build and run everything
./run.sh 02_floats       # just one program
./isa/run.sh -v          # just the instruction probe, showing every check
```

Alongside the differential programs there is `isa/`, an instruction-level
probe. The differential programs only reach what the compiler chooses to emit
from ordinary C; the probe reaches the rest, writing out every instruction the
RVA23U64 profile makes mandatory by hand and checking it against a value taken
from the specification. It is freestanding and needs no native build, since
each check carries its own answer. `./run.sh` runs it last.

## Requirements

This suite needs a **full RVA23 toolchain**: one whose default `-march`
includes `v`, `zvbb`, `zvkb` and `zvfhmin`, and whose headers provide
`riscv_vector.h`. It was written against

```
riscv64-unknown-linux-gnu-gcc (g4db0e8df15b) 15.3.0
-march=rv64imafdcv_..._zvbb_zve64d_zvfhmin_zvkb_zvkt_zvl128b
```

**CI cannot run this suite.** The CI toolchain is a plain riscv64gc one with
an RVV-capable assembler, which is enough to *assemble* vector instructions
but not to autovectorise or to supply the vector intrinsics. `run.sh` exits
77 when it cannot find a suitable compiler, so it is safe to invoke
unconditionally, but it is meant to be run by hand.

The CI-runnable coverage lives in `tests/unit/rvv.cpp` (inline assembly only,
so a plain `-march=rv64gcv` assembler is all it needs), `tests/unit/zcb.cpp`
and `tests/unit/rva23.cpp` (raw `.insn` encodings for Zfhmin, Zimop, Zcmop and
Zawrs, which no plain rv64gc toolchain can name), and `tests/disasm` (which
checks the printers against `objdump`).

Point `RVCC` at the compiler and `EMU` at the emulator to override the
defaults:

```
RVCC=/path/to/riscv64-unknown-linux-gnu-gcc ./run.sh
```

The emulator must be built with the vector extension on:

```
cd emulator && ./build.sh --V --C --A --64
```

## What the programs cover

| program | what it exercises |
| --- | --- |
| `01_intloops` | autovectorised integer loops at every width, widening and narrowing casts, divide, shifts, compare-and-select |
| `02_floats` | single and double precision, `sqrt`/`fabs`/min/max, f32↔f64, int↔float in both directions, mask-producing compares |
| `03_memops` | the libc string and memory routines — where the fault-only-first loads actually come from — plus strided and gather-shaped access |
| `04_structs` | array-of-structs access, which is what makes the compiler reach for the segment loads and stores |
| `05_reduce` | every reduction shape at every width, including the widening dot products |
| `06_fixedpoint` | `vsmul`, the averaging add/subtract, the scaling shifts and the clips, across all four `vxrm` rounding modes, plus integer divide |
| `07_permute` | slides, gathers, `vcompress`, `viota`, `vid`, the carry-in/carry-out forms, the mask scans and the mask-to-mask logic |
| `08_widening` | the widening integer and floating-point arithmetic, the widening reductions, `vfclass`, the reciprocal estimates, and Zvbb |
| `09_addressing` | unit-stride, strided, indexed in both orderings, segments at each field count, fault-only-first, the mask transfers, whole-register |
| `10_floatforms` | the remaining floating-point shapes the earlier programs do not reach |
| `isa/` | every mandatory instruction by hand: Zba, Zbb, Zbs, Zicond, Zcb, Zfhmin, Zfa, Zimop, Zcmop, Zawrs, Zihintpause, Zihintntl, Zicbom/Zicbop/Zicboz, Zicntr and the atomics |

The programs in `06`–`09` reach instructions no compiler emits from ordinary
C. Each of those computes its answer twice — once with a vector intrinsic and
once with a scalar reference written from the specification's pseudocode — and
prints both. The native build has no intrinsics, so it prints the reference
twice, which is what turns a disagreement between the two into a diff.
