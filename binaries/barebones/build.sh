#!/bin/bash
set -e
XPACK_PATH="$HOME/opt/xPacks/@xpack-dev-tools/riscv-none-embed-gcc/8.3.0-1.1.1/.content"

export PATH=$PATH:$XPACK_PATH/bin
export CC=$XPACK_PATH/bin/riscv-none-embed-gcc
export CXX=$XPACK_PATH/bin/riscv-none-embed-g++
#export CC=clang-11
#export CXX=clang++-11

mkdir -p build
pushd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=toolchain.cmake
make -j4
popd
