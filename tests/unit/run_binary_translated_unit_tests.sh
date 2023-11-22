FOLDER=build_tr
set -e

if command -v "riscv64-unknown-linux-gnu-gcc" &> /dev/null
then
	echo "Found custom Linux compiler"
	export RCC="riscv64-unknown-linux-gnu-gcc"
	export RCXX="riscv64-unknown-linux-gnu-g++"
elif command -v "riscv64-unknown-elf-gcc" &> /dev/null
then
	echo "Found custom Newlib compiler"
	export RCC="riscv64-unknown-elf-gcc"
	export RCXX="riscv64-unknown-elf-g++"
else
	echo "Could not find custom compiler for RV64G(b)"
	exit 1
fi


mkdir -p $FOLDER
pushd $FOLDER
cmake .. -DCMAKE_BUILD_TYPE=Debug -DRISCV_BINARY_TRANSLATION=ON -DRISCV_EXT_C=OFF -DRISCV_MEMORY_TRAPS=ON -DRISCV_THREADED=ON
make -j4
ctest --verbose -j4 . $@
popd
