# RISC-V userspace emulator library

## Installing a RISC-V GCC embedded compiler

```
$ xpm install --global @xpack-dev-tools/riscv-none-embed-gcc@latest
```
See more here: https://gnu-mcu-eclipse.github.io/install/

## Installing a RISC-V GCC linux compiler

To get C++ exceptions and other things, you will need a (limited) Linux userspace environment. You will need to build this cross-compiler yourself:

```
git clone https://github.com/riscv/riscv-gnu-toolchain.git
cd riscv-gnu-toolchain
./configure --prefix=$HOME/riscv --with-arch=rv32gc --with-abi=ilp32
make -j4
```
This will build a newlib cross-compiler with C++ exception support.

Note how the ABI is ilp32 and not ilp32d, which requires full floating-point support. Support will be added later on, and is instead emulated by the compiler.

Note that if you want a full glibc cross-compiler instead, simply appending `linux` to the make command will suffice, like so: `make linux`. Glibc is harder to support, and produces larger binaries, but will be more performant.

## Building and running test program

From one of the binary subfolders:
```
$ ./build.sh
```
Which will produce a `hello_world` binary in the sub-projects build folder.

Building the emulator (starting from project root) and booting the newlib `hello_world`:
```sh
mkdir -p build
cd build
cmake .. && make -j4
./remu ../binaries/newlib/build/hello_world
```

Building and running your own ELF files that can run in freestanding RV32IM is
quite challenging, so consult the hello world example! It's a bit like booting
on bare metal using multiboot, except you have easier access to system functions.

The newlib ELF files have much more C and C++ support, but still misses things like environment variables and such. This is a deliberate design as newlib is intended for embedded development.

## Instruction set support

The emulator currently supports RV32IMAC, however the foundation is laid for RV64IM.
Eventually F-extension will also be supported (floating point).

## Usage

Load a binary and let the machine simulate from `_start` (ELF entry-point):
```C++
#include <libriscv/machine.hpp>

template <int W>
long syscall_exit(riscv::Machine<W>& machine)
{
	printf(">>> Program exit(%d)\n", machine.cpu.reg(riscv::RISCV::REG_ARG0));
	machine.stop();
	return 0;
}

int main(int argc, const char** argv)
{
	const auto binary = <load your RISC-V ELF binary here>;

	riscv::Machine<riscv::RISCV32> machine { binary };
	// install a system call handler
	machine.install_syscall_handler(93, syscall_exit<riscv::RISCV32>);

	while (!machine.stopped())
	{
		machine.simulate();
	}
}
```

You can find details on the Linux system call ABI online as well as in the `syscall.h`
header in the src folder. You can use this header to make syscalls from your RISC-V programs.
It is the Linux RISC-V syscall ABI.

Be careful about modifying registers during system calls, as it may cause problems
in the simulated program.

## Why a RISC-V library

It's a drop-in sandbox. Perhaps you want someone to be able to execute C/C++ code on a website, safely?
