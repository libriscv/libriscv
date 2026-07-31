FOLDER=build_tr
set -e
source scripts/find_compiler.sh
#export RCC="riscv64-unknown-elf-gcc"
#export RCXX="riscv64-unknown-elf-g++"

# Disable the binary translation cache (/tmp/rvbintr-* shared objects). A cached
# translation is loaded before we consider compiling, and the cache key does not
# include CC/CFLAGS, so a stale object from an earlier run would silently hide
# changes to the code emitter. Set BINTR_CACHE=1 to opt back in.
export BINTR_CACHE=${BINTR_CACHE:-0}
# Compile translations at -O0. These are large generated C files and -O2 makes
# the test run take very long. CFLAGS is appended after the built-in -O2, so the
# last -O wins. Override by setting CFLAGS before running this script.
export CFLAGS=${CFLAGS:--O0}

mkdir -p $FOLDER
pushd $FOLDER
cmake .. -DCMAKE_BUILD_TYPE=Debug -DRISCV_BINARY_TRANSLATION=ON -DRISCV_EXT_C=ON -DRISCV_MEMORY_TRAPS=ON -DRISCV_THREADED=ON
make -j4
ctest --verbose -j4 . $@
popd
