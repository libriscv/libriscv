# RISC-V userspace emulator library

## Demonstration

You can find a live demonstration of the library here: https://droplet.fwsnet.net

Here is a multi-threaded test program: https://gist.github.com/fwsGonzo/e1a9cdc18f9da2ffc309fb9324a26c32

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

Note how the ABI is ilp32 and not ilp32d, which requires full floating-point support. Support is only halfway complete, and it is recommended to instead rely on the compiler to simulate this support for now.

Note that if you want a full glibc cross-compiler instead, simply appending `linux` to the make command will suffice, like so: `make linux`. Glibc is harder to support, and produces larger binaries, but will be more performant. It also supports threads, which is awesome to play around with.

## Building and running a test program

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

Building and running your own ELF files that can run in freestanding RV32GC is quite challenging, so consult the `barebones` example! It's a bit like booting on bare metal, except you can more easily implement system functions. The fun part is of course the extremely small binaries and total control over the environment.

The `newlib` example project have much more C and C++ support, but still misses things like environment variables and such. This is a deliberate design as newlib is intended for embedded development. It supports C++ RTTI and exceptions, and is the best middle-ground for running a fuller C++ environment that still produces small binaries.

The `full` example project uses the Linux-configured cross compiler and will expect you to implement quite a few system calls just to get into `int main()`. In addition, you will have to setup argv, env and the aux-vector. There is a helper method to do this in the src folder. If you want to implement threads to really get to all the fun stuff (like the coming C++20 coroutines), then this is where you can do that, as newlib simply won't enable threads.

And finally, the `micro` project implements the absolutely minimal freestanding RV32GC C/C++ environment. You don't have any heap, so no new/delete. And you can't printf values because you don't have a C standard library, so you can only write constant strings using the write system call. Still, the stripped binary is only 712 bytes, and will execute only 77 instructions running the whole program!

## Instruction set support

The emulator currently supports RV32IMAC, however the foundation is laid for RV64IM.
The F and D-extensions are halfway supported (32- and 64-bit floating point instructions).

## Usage

Load a binary and let the machine simulate from `_start` (ELF entry-point):
```C++
#include <libriscv/machine.hpp>

template <int W>
long syscall_exit(riscv::Machine<W>& machine)
{
	printf(">>> Program exited, exit code = %d\n", machine.template sysarg<int> (0));
	machine.stop();
	return 0;
}

int main(int /*argc*/, const char** /*argv*/)
{
	const auto binary = <load your RISC-V ELF binary here>;

	riscv::Machine<riscv::RISCV32> machine { binary };
	// install a system call handler
	machine.install_syscall_handler(93, syscall_exit<riscv::RISCV32>);

	// add program arguments on the stack
	std::vector<std::string> args = {
		"hello_world", "test!"
	};
	machine.setup_argv(args);

	// this function will run until the exit syscall has stopped the machine
	// or an exception happens which stops execution
	machine.simulate();
}
```

You can limit the amount of (virtual) memory the machine can use like so:
```C++
	const uint32_t max_memory = 1024 * 1024 * 64;
	riscv::Machine<riscv::RISCV32> machine { binary, max_memory };
```

You can limit the amount of instructions to simulate at a time like so:
```C++
	const uint64_t max_instructions = 1000;
	machine.simulate(max_instructions);
```
Similarly, when making a function call into the VM you can also add this limit as the last parameter to the `vmcall()` function.

You can find details on the Linux system call ABI online as well as in the `syscalls.hpp`, and `syscalls.cpp` files in the src folder. You can use these examples to handle system calls in your RISC-V programs. The system calls is emulate normal Linux system calls, and is compatible with a normal Linux RISC-V compiler.


## Tutorials

[System calls](docs/SYSCALLS.md)

[Freestanding environment](docs/FREESTANDING.md)

[Function calls into the VM](docs/VMCALL.md)


## Why a RISC-V library

It's a drop-in sandbox. Perhaps you want someone to be able to execute C/C++ code on a website, safely?

See the `webapi` folder for an example web-server that compiles and runs limited C/C++ code in a relatively safe manner. Ping me or create a PR if you notice something is exploitable.

Note that the web API demo uses a docker container to build RISC-V binaries, for security reasons. You can build the container with `docker build -t newlib-rv32gc . -f newlib.Dockerfile` from the docker folder. Alternatively, you could build a more full-fledged Linux environment using `docker build -t linux-rv32gc . -f linux.Dockerfile`. There is a test-script to see that it works called `dbuild.sh` which takes an input code file and output binary as parameters.
