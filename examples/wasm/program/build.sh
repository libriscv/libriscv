#!/bin/bash
LUAJIT=$PWD/LuaJIT

riscv64-unknown-elf-g++ -O2 -static \
  main.cpp \
  -L$LUAJIT/src \
  -l:libluajit.a \
  -I$LUAJIT/src \
  -o program.elf \
  -Wl,--undefined=run

riscv64-unknown-elf-strip --strip-unneeded program.elf -K run
