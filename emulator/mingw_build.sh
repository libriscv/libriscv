#!/usr/bin/env bash
set -e

mkdir -p .build_mingw
pushd .build_mingw
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=mingw_toolchain.cmake -DSTATIC_BUILD=ON
make -j6
popd
