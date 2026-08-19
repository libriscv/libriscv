#!/usr/bin/env bash
# Multi-buffer SHA-256 across the two axes this benchmark exists to measure:
#
#   single vs wide    vl = 1 against vl = VLMAX. The instruction stream is
#                     identical, so a flat blocks/lane-s means the emulator
#                     charges the same for a vector instruction no matter how
#                     many elements it touches, and wide gets VLMAX times the
#                     throughput for free.
#   interp vs jit     the same program with the binary translator off (-n)
#                     and on. The emulator must already be built with libtcc.
#
# The guest times itself with CLOCK_MONOTONIC around the hashing loop only,
# so neither ELF loading nor the translator's compile pass is counted.
#
#   ./run.sh                 # all four
#   ITERS=200000 ./run.sh    # longer runs
#   REPS=5 ./run.sh          # more repeats; the best of each set is kept
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
EMU=${EMU:-$HERE/../../../emulator/rvlinux}
PROG=$HERE/build/sha256_rvv.rv
ITERS=${ITERS:-100000}
REPS=${REPS:-3}

[ -x "$PROG" ] || "$HERE/build.sh"
if [ ! -x "$EMU" ]; then
	echo "no emulator at $EMU -- build it with emulator/build.sh --V --jit" >&2
	exit 77
fi

# The guest's last line: time <secs> s  hashes <n>  <MH/s> MH/s  ...  <blocks/lane-s>
run() { # mode, translate-flag
	"$EMU" --silent $2 "$PROG" "$1" "$ITERS" 2>/dev/null | awk '
		/^verify: ok/  { ok = 1 }
		/^time /       { secs = $2; mhs = $6; lanesec = $10 }
		END { if (!ok) { print "VERIFY-FAILED"; exit 1 }
		      printf "%s %s %s\n", secs, mhs, lanesec }'
}

declare -A secs mhs lanesec
for mode in single wide; do
	for backend in interp jit; do
		# -n turns the binary translator off, leaving the plain interpreter.
		flag=""; [ "$backend" = interp ] && flag="-n"
		# Best of REPS: a run can only be slowed down by the rest of the
		# machine, so the fastest one is the least noisy estimate.
		best_m=0
		for _ in $(seq "$REPS"); do
			read -r s m l < <(run "$mode" "$flag")
			if awk -v a="$m" -v b="$best_m" 'BEGIN { exit !(a > b) }'; then
				best_m=$m; best_s=$s; best_l=$l
			fi
		done
		secs[$mode/$backend]=$best_s; mhs[$mode/$backend]=$best_m
		lanesec[$mode/$backend]=$best_l
	done
done

head=$("$EMU" --silent -n "$PROG" single 1 2>/dev/null | head -1)
echo "$head" | sed 's/ iters=.*//'
echo "iterations: $ITERS   best of $REPS"
echo
printf '%-8s %-8s %10s %12s %14s\n' mode backend secs MH/s blocks/lane-s
for mode in single wide; do
	for backend in interp jit; do
		k=$mode/$backend
		printf '%-8s %-8s %10s %12s %14s\n' \
			"$mode" "$backend" "${secs[$k]}" "${mhs[$k]}" "${lanesec[$k]}"
	done
done
echo
awk -v si="${mhs[single/interp]}" -v wi="${mhs[wide/interp]}" \
    -v sj="${mhs[single/jit]}"    -v wj="${mhs[wide/jit]}" '
BEGIN {
	printf "wide / single :  interp %.2fx   jit %.2fx\n", wi/si, wj/sj
	printf "jit / interp  :  single %.2fx   wide %.2fx\n", sj/si, wj/wi
}'
