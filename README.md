# RISC-V userspace emulator library

## Installing a RISC-V GCC compiler

```
$ xpm install --global @xpack-dev-tools/riscv-none-embed-gcc@latest
```
See more here: https://gnu-mcu-eclipse.github.io/install/

## Building and running test program

From binaries folder:
```
$ ./build.sh --build hello_world.cpp
```
Which will produce `hello_world.cpp.elf`.

Building a test program (starting from root folder):
```sh
mkdir -p build
cd build
cmake .. && make -j4
./remu ../binaries/hello_world.cpp.elf
```

Building and running your own ELF files that can run in freestanding RV32IM is
quite challenging, so consult the hello world example! It's a bit like booting
on bare metal using multiboot, except you have easier access to system functions.

## Instruction set support

The emulator currently supports RV32IM, however the foundation is laid for RV64IM.
Eventually RV32C extension will also be supported (compressed).

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
