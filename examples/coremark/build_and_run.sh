#!/bin/bash
set -e

# Download coremark-rv32g_b if it doesn't exist
if [ ! -f coremark-rv32g_b.elf ]; then
    wget https://github.com/libriscv/libriscv/releases/download/v1.0/coremark-rv32g_b -O coremark-rv32g_b.elf
fi
# Require that `file coremark-rv32g_b.elf` shows that it's a RISC-V executable
file coremark-rv32g_b.elf | grep -q "RISC-V"

mkdir -p .build
pushd .build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j4
popd

./.build/coremark-cli
