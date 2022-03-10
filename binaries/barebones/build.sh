#!/bin/bash
set -e
export CC=riscv32-unknown-elf-gcc
export CXX=riscv32-unknown-elf-g++
#export CC=clang-12
#export CXX=clang++-12

mkdir -p build
pushd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=toolchain.cmake
make -j4
popd
