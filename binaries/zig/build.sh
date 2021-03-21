#!/usr/bin/env bash
set -e
zig build-exe -target riscv64-linux main.zig
