#!/usr/bin/env bash
set -e

mkdir -p .build_mingw
pushd .build_mingw
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=mingw_toolchain.cmake
make -j6
popd

ln -fs .build_mingw/rvmicro .
ln -fs .build_mingw/rvnewlib .
ln -fs .build_mingw/rvlinux .
