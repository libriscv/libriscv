FOLDER=build_unittests_exp
set -e
export RCC="riscv64-linux-gnu-gcc-10"
export RCXX="riscv64-linux-gnu-g++-10"

if command -v "riscv64-linux-gnu-gcc-11" &> /dev/null
then
    export RCC="riscv64-linux-gnu-gcc-11"
    export RCXX="riscv64-linux-gnu-g++-11"
fi

mkdir -p $FOLDER
pushd $FOLDER
cmake .. -DCMAKE_BUILD_TYPE=Debug -DRISCV_EXPERIMENTAL=ON
make -j6
ctest --verbose . $@
popd
