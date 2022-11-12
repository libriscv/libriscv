#!/usr/bin/env bash
set -e

mkdir -p build
pushd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j6
popd

ln -fs build/nodiverge .
