libriscv emulator CLI
============================================

The emulator main source file consists of the main() function, which instantiates and runs a RISC-V machine, and a helper function for loading binaries into a C++ vector. The binary filename is taken from the first argument passed to the emulator:

```
mkdir -p build
cd build && cmake .. && make -j8
./rvlinux ../../binaries/go/example
```

You will have to build the binaries first. Each binary has its own environment that it needs to succeed. The linux emulation is the one you are most likely interested in.

## Debugging

```
DEBUG=1 ./rvlinux ../../binaries/go/example
```

Will let you step through the program instruction by instruction. By default it starts from entering `main()` if the function is found. This includes Go programs.

```
GDB=1 ./rvlinux ../../binaries/go/example
```

Will let you use GDB to remotely debug the program. Use `target remote localhost:2159` in GDB to connect:

```
gdb-multiarch riscv_binary
```
Then in GDB:
```
(gdb) target remote localhost:2159
Remote debugging using localhost:2159
```
