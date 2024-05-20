## Using _libriscv_ in a game engine

This example project shows the basics for how to use _libriscv_ in a game engine.

The [build script](build_and_run.sh) will first build the RISC-V guest program, and then the host program. If it succeeds, the program will run.


## Script_program folder

An [example program](script_program/program.cpp) that modifies some libc functions to use host-side functions with native performance.

The program showcases low-latency and full C++ support.
