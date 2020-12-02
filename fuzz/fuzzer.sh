#export CXX=$HOME/llvm/install/bin/clang++-10
export ASAN_OPTIONS=disable_coredump=0::unmap_shadow_on_exit=1::handle_segv=0::handle_sigfpe=0
#export ASAN_SYMBOLIZER_PATH=$HOME/llvm/install/bin/llvm-symbolizer

set -x
set -e
mkdir -p build
pushd build
cmake ..
make -j4
popd
./build/fuzzer -N4 -handle_fpe=0
