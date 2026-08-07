FOLDER=build_asmjit
set -e
source scripts/find_compiler.sh

mkdir -p $FOLDER
pushd $FOLDER
cmake .. -DCMAKE_BUILD_TYPE=Debug -DRISCV_ASMJIT=ON -DRISCV_BINARY_TRANSLATION=OFF -DRISCV_EXT_C=ON -DRISCV_MEMORY_TRAPS=ON -DRISCV_THREADED=ON
make -j4
ctest --verbose -j4 . $@
popd
