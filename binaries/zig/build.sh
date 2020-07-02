#!/usr/bin/env bash
set -e
zig build-exe -target riscv32-freestanding main.zig --c-source start.c
