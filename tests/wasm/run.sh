#!/usr/bin/env bash
# Usage after activating emsdk: ./run.sh [--wasm-exceptions]
set -e
EH="-fexceptions"
if [ "$1" == "--wasm-exceptions" ]; then EH="-fwasm-exceptions"; fi
BUILD=.build${EH/-f/-}
mkdir -p $BUILD
pushd $BUILD
emcmake cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="$EH" -DCMAKE_EXE_LINKER_FLAGS="$EH"
make -j4
popd
echo "Run: node $BUILD/difftest.js program.elf [args...]"
