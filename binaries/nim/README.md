## Nim RISC-V program example

The program is designed to print a JSON structure and then crash.
The signal handlers will show the backtrace as long as debug info is added.

### Requirements

You will need to have Nim in your PATH.

Install gcc-10-riscv64-linux-gnu to be able build the Nim program for RISC-V.

### Debugging

Run `bash debug.sh` to start remotely debugging with GDB. You will need to build the RISC-V GDB from the RISC-V GNU toolchain, as it is not provided by any distro that I know of.
