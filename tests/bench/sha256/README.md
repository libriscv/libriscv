# Multi-buffer SHA-256 (RVV)

One SHA-256 message per vector lane, so the lane count is the only thing that
moves between the two configurations this benchmark exists to compare:

| mode | vl | what it measures |
| --- | --- | --- |
| `single` | 1 | one message in flight — every vector instruction retires exactly one element |
| `wide` | VLMAX | one message per lane — the same instruction stream, VLMAX times the work |

The lane count is a runtime value, so both modes run the *same* code through
the *same* number of instructions; only the `vsetvli` at the top differs. What
separates them is therefore the emulator's per-element cost measured against
its per-instruction dispatch cost, with nothing else changing.

Each configuration is then run twice — once with the binary translator off
(`rvlinux -n`, plain interpreter) and once with it on (libtcc) — which is the
second axis.

```
./build.sh                    # needs an RVV toolchain, see below
./run.sh                      # all four combinations
ITERS=400000 REPS=5 ./run.sh  # longer, quieter runs
```

## Requirements

`build.sh` needs a toolchain whose default `-march` includes `v` and which
ships `riscv_vector.h`; it uses `zvbb`'s `vror` for the rotates when the
toolchain offers it, and falls back to `vsll`/`vsrl`/`vor` when it does not,
which roughly doubles the instruction count. The same
`riscv64-unknown-linux-gnu-gcc` 15.3.0 that `tests/rva23` wants will do.
Point `RVCC` at it to override.

`run.sh` needs `emulator/rvlinux` built with the vector extension and libtcc:

```
cd emulator && ./build.sh --V --C --A --64 --jit
```

Point `EMU` elsewhere to override. Both scripts exit 77 when their tool is
missing, so they are safe to invoke unconditionally.

## What it runs

`sha256_block_mb()` compresses one 64-byte block per lane. The block words are
interleaved — word *i* of lane *j* at `blk[i * vl + j]` — so each schedule word
is a single unit-stride load, which is how multi-buffer implementations lay
their input out in practice.

The compiled body holds all 24 live vectors (8 state + 16 schedule) in
registers with no spills and one `vsetvli` for the whole function:

| | count |
| --- | --- |
| `vror.vi` | 576 |
| `vxor.vv` | 576 |
| `vadd.vv` | 528 |
| `vand.vv` | 192 |
| `vor.vv` | 128 |
| `vsrl.vi` | 96 |
| `vadd.vx` | 72 |
| `vle32.v` / `vse32.v` / `vmv.v.x` | 16 / 8 / 8 |

2201 vector instructions per block, and the only thing that changes between
`single` and `wide` is how many elements each of them touches.

Correctness is checked before the timed loop: the well-known digest of `"abc"`
must come out of every lane, and out of a scalar reference compression in the
same file. Each timed iteration then feeds the previous digest back into the
first eight words of the block, so iterations genuinely depend on one another
and nothing can be hoisted or folded away.

The guest times itself with `CLOCK_MONOTONIC` around the hashing loop alone,
so neither ELF loading nor the translator's compile pass (~190 ms for this
program) lands in the measurement. The instruction counts of the two modes
differ by about 2%, all of it in the `memcpy` that chains iterations and in
setup, not in the hash.

## Reading the result

```
mode     backend        secs         MH/s  blocks/lane-s
single   interp       1.6414        0.061        60923.5
single   jit          1.4575        0.069        68611.7
wide     interp       1.5576        0.514        64202.0
wide     jit          1.3995        0.572        71453.7

wide / single :  interp 8.43x   jit 8.29x
jit / interp  :  single 1.13x   wide 1.11x
```

`blocks/lane-s` is flat across `single` and `wide`: a vector instruction costs
the emulator very nearly the same whether it touches one element or eight, so
widening is close to free and `wide` collects the full VLMAX = 8 in
throughput. That is the number to watch when changing anything in the vector
element loops — it is what a per-element regression would move first, and the
`wide / single` ratio falling below VLMAX is what it looks like.

The translator is worth only about 10% here, far less than it is on scalar
code, because at 2201 vector instructions per block almost all of the time is
inside the vector helpers that both backends call the same way. It is a useful
floor: work that makes vector instructions themselves cheaper shows up in both
columns, while work on dispatch and on the checks around them shows up mostly
in the gap between them.
