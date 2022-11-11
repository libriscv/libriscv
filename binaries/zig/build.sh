#!/usr/bin/env bash
set -e
#zig targets | jq '.cpus.riscv64 | keys'
zig build-exe -target riscv64-linux main.zig
