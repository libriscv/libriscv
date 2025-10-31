#!/bin/bash
set -e
export CC=riscv64-linux-gnu-gcc-12
export CXX=riscv64-linux-gnu-g++-12

mkdir -p build
pushd build
cmake ..
make -j4
popd
