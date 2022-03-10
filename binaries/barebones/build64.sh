#!/bin/bash
set -e
RISCV_TC=$HOME/riscv
export PATH=$PATH:$RISCV_TC/bin
export CC=$RISCV_TC/bin/riscv64-unknown-elf-gcc
export CXX=$RISCV_TC/bin/riscv64-unknown-elf-g++
#export CC=clang-12
#export CXX=clang++-12

mkdir -p build64
pushd build64
cmake .. -DRISCV_ARCH=64 -DLIBC_USE_STDLIB=ON -DCMAKE_TOOLCHAIN_FILE=toolchain.cmake
make -j4
popd
