# RISC-V userspace emulator library

_libriscv_ is a RISC-V emulator that is highly embeddable and configurable. This project is intended to be included in a CMake build system, and should not be installed anywhere. There are several CMake options that control RISC-V extensions and how the emulator behaves.

Instruction counting is used to limit the time spent executing code and can be used to prevent infinite loops. It can also help keep frame budgets for long running background scripting tasks as running out of instructions simply halts execution, and it can be resumed from where it stopped.

While this emulator has a focus on performance, one higher priority is the ability to map any memory anywhere with permissions, custom fault handlers and such things. This allows you to take the memory of one machine and map it into another, and does have a slight performance penalty compared to an emulator that can only have sequential memory.

The emulator has a binary translation mode that has currently only been tested on Linux, but the performance should be good enough with the default build options.

[![Build configuration matrix](https://github.com/fwsGonzo/libriscv/actions/workflows/buildconfig.yml/badge.svg)](https://github.com/fwsGonzo/libriscv/actions/workflows/buildconfig.yml)

## Benchmarks

`fib(64000)` currently runs at 675 MIPS as measured in the [libriscv benchmark repo](https://github.com/fwsGonzo/script_bench/).

One motivation when writing this emulator was to use it in a game engine, and so it felt natural to compare against Lua, which I was already using. Lua is excellent and easy to embed, and does not require ahead-of-time compilation. However, compiled code should in theory be faster.

[STREAM memory benchmark](https://gist.github.com/fwsGonzo/a594727a9429cb29f2012652ad43fb37)

[Lua 5.4 vs Interpreted RISC-V](https://gist.github.com/fwsGonzo/2f4518b66b147ee657d64496811f9edb)

[LuaJIT vs Interpreted RISC-V](https://gist.github.com/fwsGonzo/d7ee7acb52b11ef5a51982d5b46734ca)

[LuaJIT vs Binary Translated RISC-V](https://gist.github.com/fwsGonzo/c77befe81c5957b87b96726e98466946)

## Installing a RISC-V GCC compiler

On Ubuntu and Linux distributions like it, you can install a 64-bit RISC-V GCC compiler for running Linux programs with a one-liner:

```
sudo apt install gcc-10-riscv64-linux-gnu g++-10-riscv64-linux-gnu
```

Now you have a full C/C++ compiler for RISC-V. It is typically configured to use the C-extension, so make sure you have that enabled. Note that this compiler has been built with the C-extension enabled for compressed instructions (and as a result produces smaller executables), which is not the fastest mode for emulation.

To build smaller and leaner programs you will need a (limited) Linux userspace environment. You sometimes need to build this cross-compiler yourself:

```
git clone https://github.com/riscv/riscv-gnu-toolchain.git
cd riscv-gnu-toolchain
./configure --prefix=$HOME/riscv --with-arch=rv32g --with-abi=ilp32d
make
```
This will build a newlib cross-compiler with C++ exception support. The ABI is ilp32d, which is for 32-bit and 64-bit floating-point instruction set support. It is much faster than software implementations of binary IEEE floating-point arithmetic.

Note that if you want a full glibc cross-compiler instead, simply appending `linux` to the make command will suffice, like so: `make linux`. Glibc produces larger binaries but has more features, like threads.

```
git clone https://github.com/riscv/riscv-gnu-toolchain.git
cd riscv-gnu-toolchain
./configure --prefix=$HOME/riscv --with-arch=rv64g --with-abi=lp64d
make
```
The incantation for 64-bit RISC-V. Not enabling the C-extension for compressed instructions results in faster emulation.

The last step is to add your compiler to PATH so that it becomes visible to build systems. So, add this at the bottom of your `.bashrc` file in the home (~) directory:

```
export PATH=$PATH:$HOME/riscv/bin
```

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
./rvlinux ../../binaries/linux64/build/hello_world
```

The emulator is built 3 times for different purposes. `rvmicro` is built for micro-environments with custom heap and threads. `rvnewlib` has hooked up enough system calls to run newlib. `rvlinux` has all the system calls necessary to run a normal userspace linux binary.

## Example programs

Building and running your own ELF files that can run in freestanding RV32G is quite challenging, so consult the `barebones` example! It's a bit like booting on bare metal, except you can more easily implement system functions. The fun part is of course the extremely small binaries and total control over the environment.

The `newlib` example projects have much more C and C++ support, but still misses things like environment variables and such. This is a deliberate design as newlib is intended for embedded development. It supports C++ RTTI and exceptions, and is the best middle-ground for running a fuller C++ environment that still produces small binaries.

The `linux` example projects use the Linux-configured cross compiler and will expect you to implement quite a few system calls just to get into `int main()`. In addition, you will have to setup argv, env and the aux-vector. There are helper methods to do all of this, and you should have a look at the emulator on how to set it up.

Finally, the `micro` project implements the absolutely minimal freestanding RV32GC C/C++ environment. You won't have a heap implementation, so no new/delete. And you can't printf values because you don't have a C standard library, so you can only write strings and buffers using the write system call. Still, the stripped binary is only 784 bytes, and will execute only ~120 instructions running the whole program! The `micro` project actually initializes zero-initialized memory, calls global constructors and passes program arguments to main.

## Building for Windows

There is a `build_mingw.sh` script that can build the emulator for MinGW 64 when cross-compiling on Linux. To be able to build it, install the `g++-mingw-w64-x86-64` package. If you are building inside MinGW 64 on (actual) Windows, then you should not have to do anything special. You will need the common build tools like make, cmake and a C++17-capable compiler.

## Remote debugging using GDB

If you have built the emulator, you can use `DEBUG=1 ./emulator /path/to/program` to enable GDB to connect. Most distros have `gdb-multiarch`, which is a separate program from the default gdb. It will have RISC-V support already built in. Start your GDB like so: `gdb-multiarch /path/to/program`. Make sure your program is built with -O0 and with debuginfo present. Then, once in GDB connect with `target remote localhost:2159`. Now you can step through the code.

Most modern languages embed their own pretty printers for debuginfo which enables you to go line by line in your favorite language.

## Instruction set support

The emulator currently supports RV32GC, RV64GC (IMAFDC) and RV128G.
The F and D-extensions should be 100% supported (32- and 64-bit floating point instructions). Atomics support is present and has been tested with multiprocessing, but there is no extensive test suite. The Golang runtime uses atomics extensively.

The 128-bit ISA support is experimental, and the specification is not yet complete. There is neither toolchain support, nor is there an ELF format for 128-bit machines. There is an emulator that specifically runs a custom crafted 128-bit program in the emu128 folder.

Note: There is no support for the B-, E-, V- and Q-extensions. Binary translation currently only supports RV32G and RV64G.

## Example usage when embedded into a project

Load a Linux program built for RISC-V and run through main:
```C++
#include <libriscv/machine.hpp>

int main(int /*argc*/, const char** /*argv*/)
{
	// Load ELF binary from file
	const std::vector<uint8_t> binary /* = ... */;

	using namespace riscv;

	// Create a 64-bit machine (with default options, see: libriscv/common.hpp)
	Machine<RISCV64> machine { binary };

	// Add program arguments on the stack, and set a few basic
	// environment variables.
	machine.setup_linux(
		{"myprogram", "1st argument!", "2nd argument!"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	// Add all the basic Linux system calls.
	// This includes `exit` and `exit_group` which we will override below.
	machine.setup_linux_syscalls();

	// Install our own `exit` system call handler (for all 64-bit machines).
	Machine<RISCV64>::install_syscall_handler(93, // exit
		[] (Machine<RISCV64>& machine) {
			const int code = machine.return_value <int> ();
			printf(">>> Program exited, exit code = %d\n", code);
			machine.stop();
		});
	// We also use the same system call handler again for `exit_group`,
	// which is another way that C libraries will use to end the process.
	Machine<RISCV64>::install_syscall_handler(94, // exit_group
		Machine<RISCV64>::syscall_handlers.at(93));

	// NOTE: We absolutely don't have to create our own system calls handlers,
	// but it's fun to take control of the environment inside the machine!

	// This function will run until the exit syscall has stopped the
	// machine, an exception happens which stops execution, or the
	// instruction counter reaches the given 1M instruction limit:
	try {
		machine.simulate(1'000'000UL);
	} catch (const std::exception& e) {
		fprintf(stderr, ">>> Runtime exception: %s\n", e.what());
	}
}
```

In order to have the machine not throw an exception when the instruction limit is reached, you can call simulate with the template argument false, instead:

```C++
machine.simulate<false>(1'000'000UL);
```
If the machine runs out of instructions, it will simply stop running. Use `machine.instruction_limit_reached()` to check if the machine stopped running because it hit the instruction limit.

You can limit the amount of (virtual) memory the machine can use like so:
```C++
	const uint32_t memsize = 1024 * 1024 * 64u;
	riscv::Machine<riscv::RISCV32> machine { binary, { .memory_max = memsize } };
```

You can limit the amount of instructions to simulate at a time like so:
```C++
	const uint64_t max_instructions = 2500;
	machine.simulate(max_instructions);
```
If the simulator runs out of instructions it will throw an exception. The exception is harmless and is only inteded to inform that the task took too long to complete. It is possible to keep calling `simulate()` until the machine is finished running. It is finished running when the call to simulate does not throw an exception.

When making a function call into the VM you can also add this limit as a template parameter to the `vmcall()` function.

You can find details on the Linux system call ABI online as well as in the `syscalls.hpp`, and `syscalls.cpp` files in the src folder. You can use these examples to handle system calls in your RISC-V programs. The system calls is emulate normal Linux system calls, and is compatible with a normal Linux RISC-V compiler.

## Handling instructions one by one

You can create your own custom instruction loop if you want to do things manually by yourself:

```C++
#include <libriscv/machine.hpp>
#include <libriscv/rv32i_instr.hpp>
...
auto& cpu = machine.cpu;
while (!machine.stopped()) {
	// Get 32- or 16-bits instruction
	auto instr = cpu.read_next_instruction();
	// Decode instruction to get instruction info
	auto handlers = cpu.decode(instr);
	if (false) {
		// Print instruction to terminal
		auto assembly = cpu.to_string(instr, handlers);
		printf("%.*s\n", (int)assembly.size(), assembly.c_str());
	}
	// Execute one instruction, and increment PC
	handlers.handler(cpu, instr);
	cpu.increment_pc(instr.length());
}
```
NOTE: Make sure to disable RISCV_EXPERIMENTAL when doing this. One of the experimental features enables a mode that only allows a single execute segment, and disallowing you to jump outside of it. It exists purely to improve performance of a standard static RISC-V executable.

## Setting up your own machine environment

You can create a 64kb machine without a binary, and no ELF loader will be invoked. Make sure you disable experimental features, as they may require you to set up execute segments before you can execute code.

```C++
	std::string_view empty;
	riscv::Machine<riscv::RISCV32> machine { empty, { .memory_max = 65536u }};
```

Now you can copy your machine code directly into memory:
```C++
	std::vector<uint8_t> my_program;
	const uint32_t dst = 0x1000;
	machine.copy_to_guest(dst, my_program.data(), my_program.size());
	// We will be making it execute-only (although you may want to enable read)
	machine.memory.set_page_attr(dst, my_program.size(),
		{.read = false, .write = false, .exec = true});
```

Finally, let's jump to the program entry, and start execution:
```C++
	// Example PC start address
	machine.cpu.jump(0x1068);

	// Geronimo!
	machine.simulate();
```

The fuzzing program does this, so have a look at that.

## Documentation

[System calls](docs/SYSCALLS.md)

[Freestanding environments](docs/FREESTANDING.md)

[Function calls into the VM](docs/VMCALL.md)

[Debugging in the VM](docs/DEBUGGING.md)


## Why a RISC-V library

It's a drop-in sandbox. Perhaps you want someone to be able to execute C/C++ code on a website, safely?

See the `webapi` folder for an example web-server that compiles and runs limited C/C++ code in a relatively safe manner. Ping me or create a PR if you notice something is exploitable.

Note that the web API demo uses a docker container to build RISC-V binaries, for security reasons. You can build the container with `docker build -t newlib-rv32gc . -f newlib.Dockerfile` from the docker folder. Alternatively, you could build a more full-fledged Linux environment using `docker build -t linux-rv32gc . -f linux.Dockerfile`. There is a test-script to see that it works called `dbuild.sh` which takes an input code file and output binary as parameters.

It can also be used as a script backend for a game engine, as it's quite a bit faster than Lua, although it requires you to compile the scripts ahead of time as binaries using any computer language which can output RISC-V. Check out the benchmarks section.

## What to use for performance

Use Clang (newer is better) to compile the emulator with. It is faster on most benchmarks.

Use GCC to build the RISC-V binaries, specifically the RISC-V GNU toolchain. Use -O2 or -O3 and use the regular standard extensions: `-march=rv64gc -mabi=lp64d`. Enable the RISCV_EXPERIMENTAL option for the best performance, unless you are adding your own execute pages. Use `-march=rv64g` for the absolute best performance, if you have that choice. Difference is ~5% so don't go out of your way to build everything yourself. Always enable the instruction decoder cache as it makes decoding much faster at the cost of extra memory. Always enable LTO if you can, both in the guest program and for the emulator.

Building the fastest possible RISC-V binaries for libriscv is a hard problem, but I am working on that in my [rvscript](https://github.com/fwsGonzo/rvscript) repository. It's a complex topic that cannot be explained in one paragraph.

If you have arenas available you can replace the default page fault handler with your own that allocates faster than regular heap. If you intend to use many (read hundreds, thousands) of machines in parallel, you absolutely must use the forking constructor option. It will apply copy-on-write to all pages on the newly created machine and share text, rodata and the instruction cache. Also, enable RISCV_EXPERIMENTAL so that the decoder cache will be generated ahead of time.

## Multiprocessing

There is multiprocessing support, but it is in its early stages. It is achieved by calling a (C/SYSV ABI) function on many machines, with differing CPU IDs. The input data to be processed should exist beforehand. It is not well tested, and potential page table races are not well understood. That said, it passes manual testing and there is a unit test for the basic cases. With multiprocessing I was able to achieve 3x speedup using 4 CPUs for 8192 dot-product calculations.

## Binary translation

Instead of JIT, the emulator supports translating binaries to native code using any local C or C++ compiler. You can control compilation by passing CC and CFLAGS environment variables to the program that runs the emulator. You can show the compiler arguments using VERBOSE=1. Example: `CFLAGS=-O2 VERBOSE=1 ./myemulator`.

The binary translation feature (accessible by enabling the RISCV_EXPERIMENTAL CMake option) can greatly improve performance in some cases, but requires compiling the program on the first run. The RISC-V binary is scanned for code blocks that are safe to translate, and then a C compiler is invoked on the generated code. This step takes a long time. The resulting code is then dynamically loaded and ready to use. The feature is a work in progress.
