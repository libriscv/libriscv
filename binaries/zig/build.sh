#!/usr/bin/env bash
zig build-exe -target riscv32-freestanding test.zig --c-source start.c
