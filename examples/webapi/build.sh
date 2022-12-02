#!/bin/bash
git submodule update --init
set -e

mkdir -p build
pushd build
cmake ..
make -j6
popd

./build/webapi
