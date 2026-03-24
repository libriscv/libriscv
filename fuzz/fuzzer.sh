export ASAN_OPTIONS=disable_coredump=0::unmap_shadow_on_exit=1::handle_segv=0::handle_sigfpe=0
export CC=clang-18
export CXX=clang++-18

set -e
mkdir -p build
pushd build
cmake .. -DRISCV_128I=ON -DRISCV_BINARY_TRANSLATION=OFF
make -j4
popd

echo "Example: ./build/vmfuzzer32 -fork=1 -handle_fpe=0"
