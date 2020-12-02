#!/usr/bin/env bash
set -e

mkdir -p build
pushd build
cmake .. -DRISCV_DEBUG=OFF -DRISCV_ICACHE=ON
make -j
popd
mkdir -p build_debug
pushd build_debug
cmake .. -DRISCV_DEBUG=ON
make -j
popd
