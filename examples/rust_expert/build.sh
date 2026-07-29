#!/bin/bash
set -e

GUEST=guest/target/riscv64gc-unknown-linux-gnu/release/program

pushd guest
cargo build --release
popd

mkdir -p host/.build
pushd host/.build
cmake .. -DCMAKE_BUILD_TYPE=Release -DRISCV_VIRTUAL_PAGING=OFF -DRISCV_BINARY_TRANSLATION=OFF -DRISCV_32I=OFF -DRISCV_64I=ON
make -j$(nproc)
popd

./host/.build/host_app $GUEST
