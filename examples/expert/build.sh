#!/bin/bash
set -e

mkdir -p guest/.build
pushd guest/.build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=../toolchain.cmake
make -j$(nproc)
popd

mkdir -p host/.build
pushd host/.build
cmake .. -DCMAKE_BUILD_TYPE=Release -DRISCV_VIRTUAL_PAGING=OFF -DRISCV_BINARY_TRANSLATION=OFF -DRISCV_32I=OFF -DRISCV_64I=ON
make -j$(nproc)
popd

./host/.build/host_app ./guest/guest
