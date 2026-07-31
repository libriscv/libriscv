#!/bin/bash
# Run one of the fuzzers built by fuzzer.sh.
#
# Usage:
#   ./do_fuzz.sh [target] [-- extra libfuzzer args]
#
# Examples:
#   ./do_fuzz.sh                                # vmfuzzer32
#   ./do_fuzz.sh vmfuzzer64                     # 64-bit instruction set
#   ./do_fuzz.sh elffuzzer64 -- -max_total_time=60
set -e

TARGET="${1:-vmfuzzer32}"
shift $(( $# < 1 ? $# : 1 )) || true
if [ "${1:-}" = "--" ]; then shift; fi

HERE="$(cd "$(dirname "$0")" && pwd)"

# The sanitizers and libFuzzer shell out to llvm-symbolizer to print NEW_FUNC
# lines and stack traces. On distros that ship a debuginfod URL by default
# (Ubuntu does) the symbolizer's debuginfod client can hang indefinitely, and
# because libFuzzer waits on it the fuzzer produces *no output at all*. We only
# ever symbolize our own binary, which already has full debug info, so those
# lookups are useless to us anyway.
export DEBUGINFOD_URLS=

export ASAN_OPTIONS=disable_coredump=0::unmap_shadow_on_exit=1::handle_segv=0::handle_sigfpe=0

exec "$HERE/build/$TARGET" -handle_fpe=0 "$@"
