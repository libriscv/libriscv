#!/usr/bin/env bash
set -e

mkdir -p build
pushd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DRISCV_EXT_C=OFF -DRISCV_EXPERIMENTAL=ON
make -j6
popd

./build/rvdoom
