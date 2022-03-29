FOLDER=build_unittests_exp
set -e

mkdir -p $FOLDER
pushd $FOLDER
cmake .. -DCMAKE_BUILD_TYPE=Debug -DRISCV_EXPERIMENTAL=ON
make -j6
ctest --verbose .
popd
