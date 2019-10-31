#!/bin/bash
set -e

RISCV_TC=$HOME/riscv
export PATH=$PATH:$RISCV_TC/bin
export CC=$RISCV_TC/bin/riscv32-unknown-elf-gcc
export CXX=$RISCV_TC/bin/riscv32-unknown-elf-g++

mkdir -p build
pushd build
cmake .. -D CMAKE_TOOLCHAIN_FILE=toolchain.cmake
make -j4
popd
