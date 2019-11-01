#! /bin/bash
set -e
BINARY=$1

echo -e "\n>>> Installing dependencies"
sudo pip install setuptools wheel
pip -q install gprof2dot
sudo apt install -y graphviz

echo -e "\n>>> Generating callgraph.svg from $BINARY"
gprof $BINARY | gprof2dot | dot -Tsvg -o callgraph.svg
firefox callgraph.svg
