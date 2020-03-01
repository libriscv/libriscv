#! /bin/bash
set -e
BINARY=$1

echo -e "\n>>> Installing dependencies"
sudo pip3 install setuptools wheel
pip3 -q install gprof2dot
sudo apt install -y graphviz

echo -e "\n>>> Generating callgraph.svg from $BINARY"
gprof $BINARY | gprof2dot | dot -Tsvg -o callgraph.svg
firefox callgraph.svg
