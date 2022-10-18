#!/bin/bash
set -e

GCC_TRIPLE="riscv32-unknown-elf"

RISCV_TC=$HOME/riscv
export PATH=$PATH:$RISCV_TC/bin
export CC=$RISCV_TC/bin/$GCC_TRIPLE-gcc
export CXX=$RISCV_TC/bin/$GCC_TRIPLE-g++
#export CC=riscv64-linux-gnu-gcc
#export CXX=riscv64-linux-gnu-g++

mkdir -p build
pushd build
cmake .. -DGCC_TRIPLE=$GCC_TRIPLE -DCMAKE_TOOLCHAIN_FILE=toolchain.cmake $@
make -j4
popd
