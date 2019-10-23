#!/bin/bash
set -e

export PATH=$PATH:$HOME/opt/xPacks/@xpack-dev-tools/riscv-none-embed-gcc/8.2.1-3.1.1/.content/bin
export CC=$HOME/opt/xPacks/@xpack-dev-tools/riscv-none-embed-gcc/8.2.1-3.1.1/.content/bin/riscv-none-embed-gcc
export CXX=$HOME/opt/xPacks/@xpack-dev-tools/riscv-none-embed-gcc/8.2.1-3.1.1/.content/bin/riscv-none-embed-g++

OPTS="-O1"
WARNS="-Wall -Wextra"
ABI="-march=rv32im -mabi=ilp32"
CFLAGS="$CXX -std=c++17 -static -nostdlib"
#CFLAGS="$CC -static"
# Goal: rv32ig --> rv32igc (rv32imafdc)

for i in "$@"
do
case $i in
    --build)
    shift # past argument with no value
	BLINE="$CFLAGS $ABI $WARNS $OPTS $1 start.cpp -o $1.elf"
	echo "$BLINE"
	$BLINE
	exit 0
    ;;
    --dump)
	shift
	riscv-none-embed-readelf -a $1.elf -x .rodata
	riscv-none-embed-objdump -d $1.elf
    exit 0 # done here
    ;;
    --sanitize)
    OPTS="$OPTS -fsanitize=address"
    shift # past argument with no value
    ;;
	--clean)
	shift
    rm -f $1.elf
    exit 0
    ;;
    *)
          # unknown option
		  echo "--dump, --sanitize, --clean"
    ;;
esac
done
