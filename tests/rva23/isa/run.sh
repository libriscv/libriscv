#!/usr/bin/env bash
# Instruction-level probe for the RVA23U64 mandatory extensions.
#
# The differential programs in ../progs only reach what the compiler chooses
# to emit from ordinary C. This runs the rest: every mandatory instruction
# written out by hand and checked against the specification.
#
#   ./run.sh              # build and run
#   ./run.sh -v           # show every check, not just the failures
#
# Unlike the differential suite this needs no native build, because each
# check carries its own expected value -- but it still needs an assembler
# that knows RVA23, which is the same toolchain ../run.sh asks for.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
RVCC=${RVCC:-$HOME/riscv/bin/riscv64-unknown-linux-gnu-gcc}
EMU=${EMU:-$HERE/../../../emulator/rvlinux}
OUT=$HERE/build

verbose=0
[ "${1:-}" = "-v" ] && verbose=1

if [ ! -x "$RVCC" ] && ! command -v "$RVCC" >/dev/null; then
	echo "no RVA23 toolchain at $RVCC (set RVCC)" >&2
	exit 77
fi
if [ ! -x "$EMU" ]; then
	echo "no emulator at $EMU -- build it with emulator/build.sh --V" >&2
	exit 77
fi

mkdir -p "$OUT"

# Freestanding, because the RVA23 toolchain's glibc is built for the vector
# extension and linking it would drag RVV into a test about the scalar
# extensions. -mno-relax keeps the linker from turning static data
# references into gp-relative ones, which reach past gp's +/-2KB window once
# the conversion tables are in the binary.
if ! "$RVCC" -O1 -mno-relax -nostdlib -nostartfiles -static -ffreestanding \
	-o "$OUT/probe.rv" "$HERE/rva23_isa.c" 2>"$OUT/build.log"; then
	echo "FAIL: could not build the probe"
	sed 's/^/    /' "$OUT/build.log"
	exit 1
fi

"$EMU" --silent "$OUT/probe.rv" > "$OUT/probe.out" 2>"$OUT/probe.err"
status=$?

if [ $verbose -eq 1 ]; then
	cat "$OUT/probe.out"
else
	grep -E "FAIL|all checks passed|FAILURES" "$OUT/probe.out"
fi

# A trap leaves the line naming the instruction unterminated, so the last
# line of output identifies what the emulator does not implement.
if ! grep -q "all checks passed" "$OUT/probe.out"; then
	echo "---"
	if grep -q FAILURES "$OUT/probe.out"; then
		echo "some checks computed the wrong answer"
	else
		echo "stopped early -- the emulator trapped on:"
		tail -1 "$OUT/probe.out" | sed 's/^/    /'
		grep -E "exception|Illegal|Unimplemented" "$OUT/probe.err" \
			| head -3 | sed 's/^/    /'
	fi
	exit 1
fi
exit $((status != 0))
