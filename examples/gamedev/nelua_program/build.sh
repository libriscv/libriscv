#!/usr/bin/env bash
set -e
CC=riscv64-unknown-elf-gcc
source="${1:-program}"

cfile=.build/output.c
binfile=.build/output.elf
mkdir -p .build

VERBOSE=1 nelua --ldflags="-static" --cflags="api.c -O2 -g3 -Wall -Wextra" -o $binfile $source.nelua
