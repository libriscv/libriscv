#!/bin/bash

export CXX=em++
export CC=emcc

mkdir -p .build
pushd .build
cmake -DCMAKE_BUILD_TYPE=Release \
	  -DCMAKE_TOOLCHAIN_FILE=../cmake/wasm.cmake \
	  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
popd
