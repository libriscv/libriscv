#!/usr/bin/env bash
set -e
#zig targets | jq '.cpus.riscv64 | keys'
zig build-exe -O ReleaseFast -target riscv64-linux example.zig
