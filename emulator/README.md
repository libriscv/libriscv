libriscv CLI emulator
============================================

The emulator main source file consists of the main() function, which instantiates and runs a RISC-V machine, and a helper function for loading binaries into a C++ vector. The binary filename is taken from the first argument passed to the emulator:

```
mkdir -p build
cd build && cmake .. && make -j8
./rvlinux ../../binaries/go/example
```

You will have to build the binaries first. Each binary has its own environment that it needs to succeed. The linux emulation is the one you are most likely interested in.
