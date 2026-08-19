#!/usr/bin/env bash
# Build the multi-buffer SHA-256 guest. Needs a toolchain whose -march
# includes v; zvbb is used for the rotates when the toolchain offers it.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
RVCC=${RVCC:-$HOME/riscv/bin/riscv64-unknown-linux-gnu-gcc}
OPT=${OPT:--O2}
OUT=$HERE/build

if [ ! -x "$RVCC" ] && ! command -v "$RVCC" >/dev/null; then
	echo "no RVV toolchain at $RVCC (set RVCC)" >&2
	exit 77
fi

mkdir -p "$OUT"
"$RVCC" $OPT -static -o "$OUT/sha256_rvv.rv" "$HERE/sha256_rvv.c"
echo "built $OUT/sha256_rvv.rv"
