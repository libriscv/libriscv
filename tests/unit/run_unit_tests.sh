FOLDER=build_unittests

mkdir -p $FOLDER
pushd $FOLDER
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j4
popd
