FOLDER=build_unittests
set -e
source scripts/find_compiler.sh

mkdir -p $FOLDER
pushd $FOLDER
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j4
ctest --verbose . $@
popd
