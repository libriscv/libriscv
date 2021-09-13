#!/usr/bin/env bash
set -e

mkdir -p build
pushd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j
popd

ln -fs build/rvmicro .
ln -fs build/rvnewlib .
ln -fs build/rvlinux .
