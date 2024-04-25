libriscv emulator CLI
============================================

This folder contains a small subproject that enables using libriscv to run RISC-V programs directly from the terminal. A command-line interface using libriscv, if you will.

The emulator [main source file](src/main.cpp) consists of the main() function, which instantiates and runs a RISC-V machine, and a helper function for loading binaries into a C++ vector. The binary filename is taken from the first argument passed to the emulator:

```
mkdir -p build
cd build && cmake .. && make -j8
./rvlinux ../../binaries/go/example
```

In order to use the CLI you will need some RISC-V programs. There are quite a few example programs in the [binaries](/binaries) folder. Linux emulation using Glibc or Newlib is what you are most likely interested in, although there are many types of binaries.

If you are looking for the internals of the RISC-V emulator, or to use it as a library, then that is shown in the [examples folder](/examples).

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
