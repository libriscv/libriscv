#!/bin/bash
set -e
export CC=riscv32-unknown-elf-gcc
export CXX=riscv32-unknown-elf-g++
#export CC=clang-12
#export CXX=clang++-12

mkdir -p build32
pushd build32
cmake .. -DRISCV_ARCH=32 -DLIBC_USE_STDLIB=OFF -DCMAKE_TOOLCHAIN_FILE=micro_toolchain.cmake
make -j4
popd
