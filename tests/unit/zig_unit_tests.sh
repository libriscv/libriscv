FOLDER=.build_zig
set -e
source scripts/find_compiler.sh

export RCC="zig cc -target riscv64-linux-musl"
export RCXX="zig c++ -target riscv64-linux-musl"

mkdir -p $FOLDER
pushd $FOLDER
cmake .. -DCMAKE_BUILD_TYPE=Debug -DRISCV_MEMORY_TRAPS=ON -DRISCV_THREADED=ON -DRISCV_FLAT_RW_ARENA=ON -DRISCV_BINARY_TRANSLATION=OFF
make -j4
ctest --verbose -j4 . $@
popd
