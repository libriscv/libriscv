#!/usr/bin/env bash
set -e

mkdir -p build
pushd build
if [ ! -f CMakeCache.txt ]; then
	cmake .. -DCMAKE_BUILD_TYPE=MinSizeRel -DRISCV_BINARY_TRANSLATION=ON -DRISCV_LIBTCC=ON
fi
make -j6
popd
ln -fs build/libtcc1.a .

#NO_TRANSLATE=1 ./build/rvdoom
VERBOSE=1 ./build/rvdoom
