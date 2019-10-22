# RISC-V userspace emulator library

## Installing a RISC-V GCC compiler

```
$ xpm install --global @xpack-dev-tools/riscv-none-embed-gcc@latest
```

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


## Usage

Load a binary and let the machine simulate from `_start` (ELF entry-point):
```C++
#include <machine.hpp>

template <int W>
long syscall_exit(riscv::Machine<W>& machine)
{
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

You can find details on the Linux system call ABI online as well as in the example program.
