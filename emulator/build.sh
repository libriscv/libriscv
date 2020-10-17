#!/usr/bin/env bash
mkdir -p build
pushd build
cmake .. -DRISCV_DEBUG=OFF
make -j
popd
mkdir -p build_debug
pushd build_debug
cmake .. -DRISCV_DEBUG=ON
make -j
popd
