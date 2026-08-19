#!/usr/bin/env bash
# Differential test: every program in progs/ is built twice -- once for
# RVA23 with the vector extension on, once natively -- and the two runs
# must print the same thing. A vector instruction that computes the wrong
# answer shows up as a diff; one that is missing shows up as a trap.
#
#   ./run.sh                 # build and run everything
#   ./run.sh 02_floats       # just one program
#
# RVCC points at a toolchain whose default -march is RVA23; the one this
# was written against is gcc 15.3.0 configured for rv64gcv_..._zvbb_zvfhmin.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
RVCC=${RVCC:-$HOME/riscv/bin/riscv64-unknown-linux-gnu-gcc}
EMU=${EMU:-$HERE/../../emulator/rvlinux}
CC=${CC:-gcc}
OUT=$HERE/build
# -ffp-contract=off matters for the comparison: RISC-V has a fused
# multiply-add and baseline x86-64 does not, so leaving contraction on lets
# the two builds round `a*b+c` differently in the tests' own scalar code.
# Fused arithmetic is then only what the test asks for explicitly, through
# fma()/fmaf() or a vector intrinsic.
OPT=${OPT:--O3 -ffp-contract=off}

if [ ! -x "$RVCC" ] && ! command -v "$RVCC" >/dev/null; then
	echo "no RVA23 toolchain at $RVCC (set RVCC)" >&2
	exit 77
fi
if [ ! -x "$EMU" ]; then
	echo "no emulator at $EMU -- build it with emulator/build.sh --V" >&2
	exit 77
fi

mkdir -p "$OUT"
pattern=${1:-}
fail=0 total=0

for src in "$HERE"/progs/*.c; do
	name=$(basename "$src" .c)
	[ -n "$pattern" ] && [ "$name" != "$pattern" ] && continue
	total=$((total + 1))

	if ! "$RVCC" $OPT -static -o "$OUT/$name.rv" "$src" -lm 2>"$OUT/$name.rvcc.log"; then
		echo "FAIL $name: RISC-V build"
		sed 's/^/    /' "$OUT/$name.rvcc.log"
		fail=$((fail + 1)); continue
	fi
	if ! $CC $OPT -o "$OUT/$name.native" "$src" -lm 2>"$OUT/$name.cc.log"; then
		echo "FAIL $name: native build"
		sed 's/^/    /' "$OUT/$name.cc.log"
		fail=$((fail + 1)); continue
	fi

	"$OUT/$name.native" > "$OUT/$name.expected" 2>&1
	# The emulator writes its own summary to stderr, so only stdout is
	# compared; a trap still shows up as truncated output.
	"$EMU" --silent "$OUT/$name.rv" > "$OUT/$name.actual" 2>"$OUT/$name.emu.log"
	status=$?

	if [ $status -ne 0 ]; then
		echo "FAIL $name: emulator exit $status"
		grep -E "exception|Illegal|Unimplemented" "$OUT/$name.emu.log" | head -3 | sed 's/^/    /'
		fail=$((fail + 1)); continue
	fi
	if ! diff -q "$OUT/$name.expected" "$OUT/$name.actual" >/dev/null; then
		echo "FAIL $name: output differs"
		diff "$OUT/$name.expected" "$OUT/$name.actual" | head -12 | sed 's/^/    /'
		fail=$((fail + 1)); continue
	fi
	echo "ok   $name"
done

# The instruction-level probe covers what no compiler emits from ordinary C:
# the mandatory extensions reached only by writing the encodings out by hand.
# It runs unless a single program was named on the command line.
if [ -z "$pattern" ]; then
	total=$((total + 1))
	if "$HERE/isa/run.sh" > "$OUT/isa.log" 2>&1; then
		echo "ok   isa (RVA23U64 mandatory instruction probe)"
	else
		echo "FAIL isa (RVA23U64 mandatory instruction probe)"
		sed 's/^/    /' "$OUT/isa.log"
		fail=$((fail + 1))
	fi
fi

echo "---"
if [ $fail -eq 0 ]; then
	echo "$total/$total passed"
else
	echo "$((total - fail))/$total passed, $fail failed"
fi
exit $((fail != 0))
