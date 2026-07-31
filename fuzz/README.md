# libriscv fuzzing system

## Building

To build all the available fuzzers run:
```
./fuzzer.sh
```

All the fuzzers are built in the build folder:
```
la -ls build/*fuzzer*
```
There are 3 fuzzers of each kind. A 32-bit, a 64--bit and a 128-bit fuzzer. Normally we would try to fuzz as much as possible, however the fuzzer is ineffective when it fuzzes different machines and loaders at the same time.

## Fuzzing

Example starting a fuzzer:
```
./do_fuzz.sh vmfuzzer32
```
`do_fuzz.sh` sets up the environment described below and forwards any arguments
after `--` to libFuzzer:
```
./do_fuzz.sh elffuzzer64 -- -max_total_time=60 -max_len=8192
```

You may want to make sure coredumps are enabled, through ASAN_OPTIONS:
```
export ASAN_OPTIONS=disable_coredump=0::unmap_shadow_on_exit=1::handle_segv=0::handle_sigfpe=0
```

[libfuzzer](https://llvm.org/docs/LibFuzzer.html) is being employed, which is a part of LLVM-Clang.

### If a fuzzer prints nothing and appears to hang

libFuzzer shells out to `llvm-symbolizer` to print `NEW_FUNC` lines (and, on a
crash, the stack trace), and it blocks while waiting for it. On distros that
ship a default debuginfod URL — Ubuntu sets
`DEBUGINFOD_URLS=https://debuginfod.ubuntu.com` from `/etc/profile.d` — the
symbolizer's debuginfod client can hang indefinitely, so the fuzzer produces no
output at all past the banner. Disable it:
```
export DEBUGINFOD_URLS=
```
The fuzzers are built with full debug info, so there is nothing to fetch anyway.
`fuzzer.sh` and `do_fuzz.sh` both set this. (`-print_funcs=0` also sidesteps the
NEW_FUNC symbolization, but then crash stack traces still hang.)

## Seeding the ELF loader fuzzers

Starting from an empty corpus, the `elffuzzer*` targets spend the entire run
failing ELF header validation. Seed them with the real RISC-V binaries in
`tests/unit/elf`, matched to the fuzzer's ELF class — measured over 25s:

| seed corpus | coverage |
| ----------- | -------- |
| elffuzzer32, none | cov: 987 |
| elffuzzer32, the 3 ELFCLASS32 binaries | cov: 7121 |
| elffuzzer64, none | cov: 987 |
| elffuzzer64, the 5 ELFCLASS64 binaries | cov: 9430 |

Pass a writable corpus directory first, then the seed directory read-only, so
the fuzzer never writes into the git-tracked binaries:
```
mkdir -p corpus/elffuzzer64 seeds/64
cp ../tests/unit/elf/*rv64* ../tests/unit/elf/*riscv64* seeds/64/
./do_fuzz.sh elffuzzer64 -- corpus/elffuzzer64 seeds/64
```

Two caveats. Do *not* set `-max_len` here: libFuzzer skips seed files larger
than it, which silently drops most of these binaries and undoes the gain. And
the large seeds trade throughput for coverage — the full rv64 set reaches
cov 9430 in only ~2.4k runs, versus cov 7900 in ~254k runs when seeded with just
the 2 KB `tinycc-rv64g-fib`. The big binaries exercise more loader paths; the
small one gets far more mutations. Pick based on what you are hunting.

`elffuzzer128` cannot be seeded from these, as it requires an `ELFCLASS128`
binary — a libriscv extension no toolchain here emits.

## Fuzzing in CI

`.github/workflows/fuzzing.yml` builds every fuzzer and runs each one for 30
seconds on push and pull request, failing the job if any target finds a crash
and uploading the reproducer as an artifact. The ELF loader targets are seeded
as described above. Use the workflow's manual *Run workflow* button to fuzz for
longer than 30 seconds.

Each run starts from an empty corpus, so CI rediscovers coverage every time. It
catches regressions; it is not a substitute for long-running local fuzzing.

## Fuzzing the ELF loader in Docker

If the native build doesn't work on your machine, `docker.sh` builds the ELF
loader fuzzers (`elffuzzer32` / `elffuzzer64`) inside a container with the
right clang-18 toolchain and runs one of them in normal libFuzzer mode:

```
./docker.sh                          # elffuzzer64 (default)
./docker.sh elffuzzer32              # 32-bit ELF loader
./docker.sh elffuzzer64 -- -max_len=8192   # forward extra libFuzzer args
```

The image seeds the corpus with a real RISC-V ELF binary (`led_hello.elf` for
rv32, `hello` for rv64) so the fuzzer gets past ELF header validation and
reaches the loader/decoder code. Note that each iteration loads and simulates a
full ELF under ASan at `-O0`, so it runs at a low exec/s — let it fuzz for a
while.

The corpus and any crash artifacts are persisted on the host under
`fuzz/corpus-<target>/`, so progress survives container restarts.
