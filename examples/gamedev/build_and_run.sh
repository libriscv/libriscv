#!/bin/bash
set -e

pushd script_program
source ./build.sh
popd

mkdir -p .build
pushd .build
cmake .. -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_UPDATES_DISCONNECTED=ON
make -j4
popd

./.build/example script_program/micro 64
