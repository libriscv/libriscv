#!/usr/bin/env bash
set -e

mkdir -p .build
pushd .build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j6
popd

if test -f ".build/rvmicro"; then
	ln -fs .build/rvmicro .
fi
if test -f ".build/rvnewlib"; then
	ln -fs .build/rvnewlib .
fi
ln -fs .build/rvlinux .
