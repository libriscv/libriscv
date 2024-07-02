#!/usr/bin/env bash
set -e

OPTS=""
EMBED_FILES=""

function usage()
{
   progname=$(basename $0)
   cat << HEREDOC

   Usage: $progname [options] [--embed file] [--embed file] ...

   optional arguments:
     -h, --help           show this help message and exit
     --A                  enable atomic extension
     --no-A               disable atomic extension
     --C                  enable compressed extension
     --no-C               disable compressed extension
     --V                  enable vector extension
     --no-V               disable vector extension
     --32                 enable RV32
     --no-32              disable RV32
     --64                 enable RV64
     --no-64              disable RV64
     --128                enable RV128
     --no-128             disable RV128
     -b, --bintr          enable binary translation using system compiler
     -t, --tcc            jit-compile using tcc
     --no-bintr           disable binary translation
     -x, --expr           enable experimental features (eg. unbounded 32-bit addressing)
     --embed FILE         embed binary translated sources into the emulator, produced by CLI -o option
     -v, --verbose        increase the verbosity of the bash script

HEREDOC
}

while [[ "$#" -gt 0 ]]; do
    case $1 in
		-h|--help ) usage; exit; ;;
		--A) OPTS="$OPTS -DRISCV_EXT_A=ON" ;;
		--no-A) OPTS="$OPTS -DRISCV_EXT_A=OFF" ;;
		--C) OPTS="$OPTS -DRISCV_EXT_C=ON" ;;
		--no-C) OPTS="$OPTS -DRISCV_EXT_C=OFF" ;;
		--V) OPTS="$OPTS -DRISCV_EXT_V=ON" ;;
		--no-V) OPTS="$OPTS -DRISCV_EXT_V=OFF" ;;
		--32) OPTS="$OPTS -DRISCV_32I=ON" ;;
		--no-32) OPTS="$OPTS -DRISCV_32I=OFF" ;;
		--64) OPTS="$OPTS -DRISCV_64I=ON" ;;
		--no-64) OPTS="$OPTS -DRISCV_64I=OFF" ;;
		--128) OPTS="$OPTS -DRISCV_128I=ON" ;;
		--no-128) OPTS="$OPTS -DRISCV_128I=OFF" ;;
        --no-bintr) OPTS="$OPTS -DRISCV_BINARY_TRANSLATION=OFF" ;;
        -b|--bintr) OPTS="$OPTS -DRISCV_BINARY_TRANSLATION=ON -DRISCV_LIBTCC=OFF" ;;
        -t|--tcc  ) OPTS="$OPTS -DRISCV_BINARY_TRANSLATION=ON -DRISCV_LIBTCC=ON" ;;
        -x|--expr ) OPTS="$OPTS -DRISCV_EXPERIMENTAL=ON -DRISCV_ENCOMPASSING_ARENA=ON" ;;
		--embed) EMBED_FILES="$EMBED_FILES;$2"; shift ;;
		-v|--verbose ) set -x ;;
        *) echo "Unknown parameter passed: $1"; exit 1 ;;
    esac
    shift
done

mkdir -p .build
pushd .build
cmake .. -DCMAKE_BUILD_TYPE=Release $OPTS -DEMBED_FILES="$EMBED_FILES"
make -j6
popd

if test -f ".build/rvmicro"; then
	ln -fs .build/rvmicro .
fi
if test -f ".build/rvnewlib"; then
	ln -fs .build/rvnewlib .
fi
if test -f ".build/libtcc1.a"; then
	ln -fs .build/libtcc1.a .
fi
ln -fs .build/rvlinux .
