#!/bin/bash
set -e

export PATH=$PATH:$HOME/opt/xPacks/@xpack-dev-tools/riscv-none-embed-gcc/8.2.1-3.1.1/.content/bin
export CC=$HOME/opt/xPacks/@xpack-dev-tools/riscv-none-embed-gcc/8.2.1-3.1.1/.content/bin/riscv-none-embed-gcc
export CXX=$HOME/opt/xPacks/@xpack-dev-tools/riscv-none-embed-gcc/8.2.1-3.1.1/.content/bin/riscv-none-embed-g++

mkdir -p build
pushd build
cmake .. -D CMAKE_TOOLCHAIN_FILE=toolchain.cmake
make -j4
popd
