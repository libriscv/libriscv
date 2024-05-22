#!/bin/bash
set -e

export ENGINE_CC="$CC"
export ENGINE_CXX="$CXX"

# The script program uses a different compiler
pushd script_program
source ./build.sh
popd

# Restore the original compiler
CC=$ENGINE_CC
CXX=$ENGINE_CXX

mkdir -p .build
pushd .build
cmake .. -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_UPDATES_DISCONNECTED=ON
make -j4
popd

#./.build/simple_example test
./.build/example script_program/micro
