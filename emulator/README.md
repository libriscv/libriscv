Emulator
============================================

This is not a general purpose emulator that runs every binary. It has several configurations and several ways to select groups of system calls that it will support. It also has a myriad of commented-out debugging-related settings that I personally use when I think there is a problem with an instruction.

Regardless, feel free to use this is a template for your own, based on your needs. Check out the src/syscalls.* and src/threads.* source files for some basic implementations and stubs of system calls needed for regular Linux emulation.

The emulator main source file consists of the main() function, which instantiates and runs a RISC-V machine, and a helper function for loading binaries into a C++ vector. The binary filename is taken from the first argument passed to the emulator:

```
mkdir -p build
cd build && cmake .. && make -j8
./remu ../../binaries/testsuite/build/testsuite
```

You will have to build the binaries first. Each binary has its own environment that it needs to succeed. The micro binaries need less and the newlib/full binaries need more/everything.
