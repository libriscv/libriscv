#!/bin/bash
set -e

mkdir -p guest/.build
pushd guest/.build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=../toolchain.cmake
make -j$(nproc)
popd

mkdir -p host/.build
pushd host/.build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
popd

./host/.build/host_app ./guest/guest
