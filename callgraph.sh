#! /bin/bash
set -e
EMULATOR="./emulator/rvlinux"
BINARY=${1:-binaries/linux64/build/hello_world}

echo -e "\n>>> Installing dependencies"
sudo pip3 install setuptools wheel
pip3 -q install gprof2dot
sudo apt install -y graphviz

echo -e "\n>>> Building RISC-V Linux emulator..."

pushd emulator
mkdir -p build_callgraph
pushd build_callgraph
cmake .. -G Ninja -DGPROF=ON -DCMAKE_BUILD_TYPE=Debug
ninja
popd

echo -e "\n>>> Done"

popd

echo -e "\n>>> Generating callgraph.svg from $BINARY"

$EMULATOR $BINARY
gprof $EMULATOR | gprof2dot | dot -Tsvg -o callgraph.svg
firefox callgraph.svg
