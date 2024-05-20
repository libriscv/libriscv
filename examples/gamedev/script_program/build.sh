#!/bin/bash
set -e

CC=riscv64-linux-gnu-gcc-12
CXX=riscv64-linux-gnu-g++-12

mkdir -p .build
pushd .build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j4
popd
