#!/bin/bash
set -e

mkdir -p build
pushd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
popd

./build/ibench $@
