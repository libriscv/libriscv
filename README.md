# RISC-V userspace emulator library

_libriscv_ is a RISC-V emulator that is highly embeddable and configurable. This project is intended to be included in a CMake build system, and should not be installed anywhere. There are several CMake options that control RISC-V extensions and how the emulator behaves.

## Benchmarks against Lua

My primary motivation when writing this emulator was to use it in a game engine, and so it felt natural to compare against Lua, which I was already using.

[Benchmarks with binary translation enabled](https://gist.github.com/fwsGonzo/c77befe81c5957b87b96726e98466946)

[LuaJIT benchmarks](https://gist.github.com/fwsGonzo/f874ba58f2bab1bf502cad47a9b2fbed)

[Lua 5.4 benchmarks](https://gist.github.com/fwsGonzo/2f4518b66b147ee657d64496811f9edb)

## Installing a RISC-V GCC compiler

To get C++ exceptions and other things, you will need a (limited) Linux userspace environment. You will need to build this cross-compiler yourself:

```
git clone https://github.com/riscv/riscv-gnu-toolchain.git
cd riscv-gnu-toolchain
./configure --prefix=$HOME/riscv --with-arch=rv32g --with-abi=ilp32d
make -j4
```
This will build a newlib cross-compiler with C++ exception support. The ABI is ilp32d, which is for 32-bit and 64-bit floating-point instruction set support. It is much faster than software implementations of binary IEEE floating-point arithmetic.

Note that if you want a full glibc cross-compiler instead, simply appending `linux` to the make command will suffice, like so: `make linux`. Glibc is harder to support, and produces larger binaries, but will be more performant. It also supports threads, which is awesome to play around with.

```
git clone https://github.com/riscv/riscv-gnu-toolchain.git
cd riscv-gnu-toolchain
./configure --prefix=$HOME/riscv --with-arch=rv64g --with-abi=lp64d
make -j4
```
The incantation for 64-bit RISC-V.

The last step is to add your compiler to PATH so that it becomes visible to build systems. So, add this at the bottom of your `.bashrc` file in the home (~) directory:

```
export PATH=$PATH:$HOME/riscv/bin
```

## Installing a RISC-V GCC embedded compiler

```
$ xpm install --global @xpack-dev-tools/riscv-none-embed-gcc@latest
```
See more here: https://gnu-mcu-eclipse.github.io/install/

This compiler will build the smallest executables. See the micro examples in the binaries folder. Don't start with this unless you already know how to write startup code.

## Building and running a test program

From one of the binary subfolders:
```
$ ./build.sh
```
Which will produce a `hello_world` binary in the sub-projects build folder.

Building the emulator and booting the newlib `hello_world`:
```sh
cd emulator
mkdir -p build && cd build
cmake .. && make -j4
./rvnewlib ../../binaries/newlib/build/hello_world
```

The emulator is built 3 times for different purposes. `rvmicro` is built for micro-environments with custom heap and threads. `rvnewlib` has hooked up enough system calls to run newlib. `rvlinux` has all the system calls necessary to run a normal userspace linux binary.

Building and running your own ELF files that can run in freestanding RV32GC is quite challenging, so consult the `barebones` example! It's a bit like booting on bare metal, except you can more easily implement system functions. The fun part is of course the extremely small binaries and total control over the environment.

The `newlib` example project have much more C and C++ support, but still misses things like environment variables and such. This is a deliberate design as newlib is intended for embedded development. It supports C++ RTTI and exceptions, and is the best middle-ground for running a fuller C++ environment that still produces small binaries.

The `full` example project uses the Linux-configured cross compiler and will expect you to implement quite a few system calls just to get into `int main()`. In addition, you will have to setup argv, env and the aux-vector. There is a helper method to do this in the src folder. There is also basic pthreads support.

And finally, the `micro` project implements the absolutely minimal freestanding RV32GC C/C++ environment. You won't have a heap implementation, so no new/delete. And you can't printf values because you don't have a C standard library, so you can only write strings and buffers using the write system call. Still, the stripped binary is only 784 bytes, and will execute only ~120 instructions running the whole program! The `micro` project actually initializes zero-initialized memory, calls global constructors and passes program arguments to main.

## Instruction set support

The emulator currently supports RV32GC, and RV64GC (IMAFDC).
The F and D-extensions should be 100% supported (32- and 64-bit floating point instructions), and there is a test-suite for these instructions, however they haven't been extensively tested as there are generally few FP-instructions in normal programs.

Note: There is no support for the B-, E- and Q-extensions.

## Usage

Load a binary and let the machine simulate from `_start` (ELF entry-point):
```C++
#include <libriscv/machine.hpp>

int main(int /*argc*/, const char** /*argv*/)
{
	// load binary from file
	const std::vector<uint8_t> binary /* = ... */;

	using namespace riscv;
	// create a 64-bit machine
	Machine<RISCV64> machine { binary };

	// install the `exit` system call handler
	machine.install_syscall_handler(93,
	 [] (auto& machine) {
		 const auto [code] = machine.template sysargs <int> ();
		 printf(">>> Program exited, exit code = %d\n", code);
		 machine.stop();
	 });

	// add program arguments on the stack
	machine.setup_argv({"emulator", "test!"});

	// this function will run until the exit syscall has stopped the
	// machine, an exception happens which stops execution, or the
	// instruction counter reaches the given limit (1M):
	try {
		machine.simulate(1'000'000);
	} catch (const std::exception& e) {
		fprintf(stderr, ">>> Runtime exception: %s\n", e.what());
	}
}
```

You can find the example above in the `emulator/minimal` folder. It's a normal CMake project, so you can build it like so:

```
mkdir -p build && cd build
cmake .. && make -j4
./emulator
```

If you run the program as-is with no program loaded, you will get an `Execution space protection fault`, which means the emulator tried to execute on non-executable memory.

```
$ ./emulator
>>> Runtime exception: Execution space protection fault
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
Similarly, when making a function call into the VM you can also add this limit as a template parameter to the `vmcall()` function.

You can find details on the Linux system call ABI online as well as in the `syscalls.hpp`, and `syscalls.cpp` files in the src folder. You can use these examples to handle system calls in your RISC-V programs. The system calls is emulate normal Linux system calls, and is compatible with a normal Linux RISC-V compiler.

## Setting up your own machine environment

You can create a 64kb machine without a binary, and no ELF loader will be invoked.
```C++
	const uint32_t max_memory = 65536;
	std::vector<uint8_t> nothing; // taken as reference
	riscv::Machine<riscv::RISCV32> machine { nothing, max_memory };
```

Now you can copy your machine code directly into memory:
```C++
	std::vector<uint8_t> my_program_data;
	const uint32_t dst = 0x1000;
	machine.copy_to_guest(dst, my_program_data.data(), my_program_data.size());
```

Finally, let's jump to the program entry, and start execution:
```C++
	// example PC start address
	const uint32_t entry_point = 0x1068;
	machine.cpu.jump(entry_point);

	// geronimo!
	machine.simulate(5'000);
```

The fuzzing program does this, so have a look at that.

## Documentation

[System calls](docs/SYSCALLS.md)

[Freestanding environment](docs/FREESTANDING.md)

[Function calls into the VM](docs/VMCALL.md)


## Why a RISC-V library

It's a drop-in sandbox. Perhaps you want someone to be able to execute C/C++ code on a website, safely?

See the `webapi` folder for an example web-server that compiles and runs limited C/C++ code in a relatively safe manner. Ping me or create a PR if you notice something is exploitable.

Note that the web API demo uses a docker container to build RISC-V binaries, for security reasons. You can build the container with `docker build -t newlib-rv32gc . -f newlib.Dockerfile` from the docker folder. Alternatively, you could build a more full-fledged Linux environment using `docker build -t linux-rv32gc . -f linux.Dockerfile`. There is a test-script to see that it works called `dbuild.sh` which takes an input code file and output binary as parameters.

It can also be used as a script backend for a game engine, as it's quite a bit faster than LuaJIT, although it requires you to compile the scripts ahead of time as binaries using any computer language which can output RISC-V.

## What to use for performance

Use Clang (newer is better) to compile the emulator with. It is somewhere between 20-25% faster on most everything.

Use GCC to build the RISC-V binaries. Use -O2 or -O3 and use the regular standard extensions: `-march=rv32gc -mabi=ilp32d`. Enable the RISCV_EXPERIMENTAL option for the best performance unless you are using libriscv as a sandbox. Use `-march=rv32g` for the absolute best performance, if you have that choice. Difference is minimal so don't go out of your way to build everything yourself. Always enable the instruction decoder cache as it makes decoding much faster at the cost of extra memory. Always enable LTO and `-march=native` if you can.

Building the fastest possible RISC-V binaries for libriscv is a hard problem, but I am working on that in my [rvscript](https://github.com/fwsGonzo/rvscript) repository. It's a complex topic that cannot be explained in one paragraph.

If you have arenas available you can replace the default page fault handler with your that allocates faster than regular heap. If you intend to use many (read hundreds, thousands) of machines in parallel, you absolutely must use the forking constructor option. It will apply copy-on-write to all pages on the newly created machine and share text and rodata. Also, enable RISCV_EXPERIMENTAL so that the decoder cache will be generated ahead of time.
