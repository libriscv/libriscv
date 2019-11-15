#!/bin/bash
DCPATH=/usr/outside
INFILE=$DCPATH/$1
OUTFILE=$DCPATH/$2
IMAGE=${3:-gcc9-rv32imac}

BC="riscv32-unknown-elf-g++ -march=rv32imc -mabi=ilp32 -static \
	-std=c++17 -O2 -fstack-protector $INFILE -o $OUTFILE \
	-ffunction-sections -fdata-sections -Wl,-gc-sections -Wl,-s"

docker run --volume $(pwd):$DCPATH -it $IMAGE $BC
