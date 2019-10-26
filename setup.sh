#!/bin/bash
# sudo apt install npm
# sudo npm install --global xpm
# xpm install --global @xpack-dev-tools/riscv-none-embed-gcc@latest
git submodule update --init --recursive
mkdir -p build
pushd build
cmake .. && make -j4
popd
