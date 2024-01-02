bin=$1
env LD_LIBRARY_PATH=/lib/libc6-prof/x86_64-linux-gnu sudo -E perf record --call-graph dwarf $bin
sudo perf report --call-graph
