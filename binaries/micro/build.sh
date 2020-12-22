#!/bin/bash
set -e

COMPILER_DIR=$HOME/opt/xPacks/@xpack-dev-tools/riscv-none-embed-gcc/8.3.0-2.3.1/.content
export PATH=$PATH:$COMPILER_DIR/bin
export CC=$COMPILER_DIR/bin/riscv-none-embed-gcc
export CXX=$COMPILER_DIR/bin/riscv-none-embed-g++

mkdir -p build
pushd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=toolchain.cmake -DCOMPILER_DIR=$COMPILER_DIR
make -j4
popd
