# RISC-V userspace emulator library

_libriscv_ is a simple, slim and complete RISC-V userspace emulator library that is highly embeddable and configurable. It is a specialty emulator that specializes in low-latency, low-footprint emulation. _libriscv_ may be the only one of its kind. Where other solutions routinely require ~50ns to enter the virtual machine and return, _libriscv_ requires 3ns. _libriscv_ has specialized APIs that make passing data in and out of the sandbox safe and low-latency.

There is also [a CLI](/emulator) that you can use to run RISC-V programs and step through instructions one by one, like a simulator, or connect with GDB.

[![Debian Packaging](https://github.com/fwsGonzo/libriscv/actions/workflows/packaging.yml/badge.svg)](https://github.com/fwsGonzo/libriscv/actions/workflows/packaging.yml) [![Build configuration matrix](https://github.com/fwsGonzo/libriscv/actions/workflows/buildconfig.yml/badge.svg)](https://github.com/fwsGonzo/libriscv/actions/workflows/buildconfig.yml) [![Unit Tests](https://github.com/fwsGonzo/libriscv/actions/workflows/unittests.yml/badge.svg)](https://github.com/fwsGonzo/libriscv/actions/workflows/unittests.yml) [![Experimental Unit Tests](https://github.com/fwsGonzo/libriscv/actions/workflows/unittests_exp.yml/badge.svg)](https://github.com/fwsGonzo/libriscv/actions/workflows/unittests_exp.yml) [![Linux emulator](https://github.com/fwsGonzo/libriscv/actions/workflows/emulator.yml/badge.svg)](https://github.com/fwsGonzo/libriscv/actions/workflows/emulator.yml) [![MinGW 64-bit emulator build](https://github.com/fwsGonzo/libriscv/actions/workflows/mingw.yml/badge.svg)](https://github.com/fwsGonzo/libriscv/actions/workflows/mingw.yml) [![Verify example programs](https://github.com/fwsGonzo/libriscv/actions/workflows/verify_examples.yml/badge.svg)](https://github.com/fwsGonzo/libriscv/actions/workflows/verify_examples.yml)

![render1702368897099](https://github.com/fwsGonzo/libriscv/assets/3758947/89d6c128-c410-4fe5-bf03-eff0279f8933)

For discussions & help, [visit Discord](https://discord.gg/aGhUSBpH).

## Ultra-Low latency emulation

libriscv is an ultra-low latency emulator, designed specifically to have very low overheads when sandboxing certain programs, such as game scripts.

Goals:
- No overhead when used for game engine scripting or request-based workloads
- Type-safe VM call and system call interfaces
- [Secure speculation-safe sandbox](SECURITY.md)
- Low attack surface, only 20k LOC
- Platform-independent and super-easy to embed

Non goals:
- Just-in-time compilation
- Wide support for Linux system calls
- Other attack surface inflators

## Benchmarks

[STREAM benchmark](https://gist.github.com/fwsGonzo/a594727a9429cb29f2012652ad43fb37) [CoreMark: 13424](https://gist.github.com/fwsGonzo/7ef100ba4fe7116e97ddb20cf26e6879) vs 41382 native.

Run [D00M 1 in libriscv](/examples/doom) and see for yourself. It should use around 8% CPU at 60 fps.

Benchmark between [binary translated libriscv vs LuaJIT](https://gist.github.com/fwsGonzo/9132f0ef7d3f009baa5b222eedf392da), [interpreted libriscv vs LuaJIT](https://gist.github.com/fwsGonzo/1af5b2a9b4f38c1f3d3074d78acdf609) and also [interpreted libriscv vs Luau](https://gist.github.com/fwsGonzo/5ac8f4d8ca84e97b0c527aec76a86fe9).  Most benchmarks are hand-picked for the purposes of game engine scripting, but there are still some classic benchmarks.

<details>
  <summary>Register vs stack machines (interpreted)</summary>
  
  ### Benchmarks against wasm3
  RISC-V is a register machine architecture, which makes it very easy to reach good interpreter performance without needing a register allocator.

  ![STREAM memory wasm3 vs  libriscv (no SIMD)](https://github.com/fwsGonzo/libriscv/assets/3758947/0a259f83-0a60-4f0d-88e8-901333ca1c7d)
  ![CoreMark 1 0 Interpreted wasm3 vs  interpreted libriscv](https://github.com/fwsGonzo/libriscv/assets/3758947/236c6620-6812-4e6c-89be-15cdb7340412)

</details>

## Embedding the emulator in a project

See the [example project](/examples/embed) for directly embedding libriscv using CMake. You can also use libriscv as a packaged artifact, through the [package example](/examples/package), although you will need to install [the package](/.github/workflows/packaging.yml) first.

On Windows you can use Clang-cl in Visual Studio. See the [example CMake project](/examples/msvc). It requires Clang and Git installed.


## Emulator using Docker CLI

```sh
docker build . -t libriscv
docker run -v $PWD/binaries:/app/binaries --rm -i -t libriscv binaries/<binary>
```

A fib(256000000) program for testing is built automatically. You can test-run it like so:
```sh
docker run -v $PWD/binaries:/app/binaries --rm -i -t libriscv fib
```

If you want to use `rvlinux` from terminal, or you want to compile RISC-V programs, you can enter the docker container instead of using it from the outside. A 64-bit RISC-V compiler is installed in the container, and it can be used to build RISC-V programs. You can enter the container like so:
```sh
docker run -v $PWD/binaries:/app/binaries --entrypoint='' -i -t libriscv /bin/bash
```

Inside the container you have access to the emulator `rvlinux`, and the compilers `riscv64-linux-gnu-gcc-12` and `riscv64-linux-gnu-g++-12`. There is also `rvlinux-fast` which cannot run RISC-V programs with compressed instructions, but is a lot faster.


## Installing a RISC-V GCC compiler

On Ubuntu and Linux distributions like it, you can install a 64-bit RISC-V GCC compiler for running Linux programs with a one-liner:

```
sudo apt install gcc-12-riscv64-linux-gnu g++-12-riscv64-linux-gnu
```

Depending on your distro you may have access to GCC versions 10 to 13. Now you have a full Linux C/C++ compiler for 64-bit RISC-V.

To build smaller and leaner programs you will want a (limited) Linux userspace environment. Check out the guide on how to [build a Newlib compiler](/docs/NEWLIB.md).

## Running a RISC-V program

```sh
cd emulator
./build.sh
./rvlinux <path to RISC-V ELF binary>
```

You can step through programs instruction by instruction by running the emulator with `DEBUG=1`:
```sh
cd emulator
DEBUG=1 ./rvlinux <path to RISC-V ELF binary>
```

You can use GDB remotely by starting the emulator with `GDB=1`:
```sh
cd emulator
GDB=1 ./rvlinux <path to RISC-V ELF binary>
```
Connect from `gdb-multiarch` with `target remote :2159` after loading the program with `file <path>`.


## Example RISC-V programs

The [binaries folder](/binaries/) contains several example programs.

The [newlib](/binaries/newlib) and [newlib64](/binaries/newlib64) example projects have much more C and C++ support, but still misses things like environment variables and such. This is a deliberate design as newlib is intended for embedded development. It supports C++ RTTI and exceptions, and is the best middle-ground for running a fuller C++ environment that still produces small binaries. You can run these programs with rvnewlib.

The [linux](/binaries/linux) and [linux64](/binaries/linux64) example projects require a Linux-configured cross compiler. You can run these programs with rvlinux.

The [Go](/binaries/go) examples only require Go installed. Go produces complex RV64G ELF executables.

There are also examples for [Nim](/binaries/nim), [Zig](/binaries/zig), [Rust](/binaries/rust) and [Nelua](/binaries/nelua).

## Remote debugging using GDB

If you have built the emulator, you can use `GDB=1 ./rvlinux /path/to/program` to enable GDB to connect. Most distros have `gdb-multiarch`, which is a separate program from the default gdb. It will have RISC-V support already built in. Start your GDB like so: `gdb-multiarch /path/to/program`. Make sure your program is built with -O0 and with debuginfo present. Then, once in GDB connect with `target remote :2159`. Now you can step through the code.

Most modern languages embed their own pretty printers for debuginfo which enables you to go line by line in your favorite language.

## Instruction set support

The emulator currently supports RV32GCB, RV64GCB (imafdc_zicsr_zifence_zba_zbb_zbc_zbs) and RV128G.
The A-, F-, D-, C- and B-extensions should be 100% supported on 32- and 64-bit. V-extension is undergoing work.

The 128-bit ISA support is experimental, and the specification is not yet complete.

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
		{"LC_TYPE=C", "LC_ALL=C", "USER=groot"});

	// Add all the basic Linux system calls.
	// This includes `exit` and `exit_group` which we will override below.
	machine.setup_linux_syscalls();

	// Install our own `exit_group` system call handler (for all 64-bit machines).
	Machine<RISCV64>::install_syscall_handler(94, // exit_group
		[] (Machine<RISCV64>& machine) {
			const auto [code] = machine.sysarg <int> (0);
			printf(">>> Program exited, exit code = %d\n", code);
			machine.stop();
		});

	// This function will run until the exit syscall has stopped the
	// machine, an exception happens which stops execution, or the
	// instruction counter reaches the given 5bn instruction limit:
	try {
		machine.simulate(5'000'000'000ull);
	} catch (const std::exception& e) {
		fprintf(stderr, ">>> Runtime exception: %s\n", e.what());
	}
}
```

In order to have the machine not throw an exception when the instruction limit is reached, you can call simulate with the template argument false, instead:

```C++
machine.simulate<false>(5'000'000ull);
```
If the machine runs out of instructions, it will now simply stop running. Use `machine.instruction_limit_reached()` to check if the machine stopped running because it hit the instruction limit.

You can limit the amount of (virtual) memory the machine can use like so:
```C++
	const uint64_t memsize = 1024 * 1024 * 64ul;
	riscv::Machine<riscv::RISCV32> machine { binary, { .memory_max = memsize } };
```
You can find the `MachineOptions` structure in [common.hpp](/lib/libriscv/common.hpp).

You can find details on the Linux system call ABI online as well as in [the docs](/docs/SYSCALLS.md). You can use these examples to handle system calls in your RISC-V programs. The system calls emulate normal Linux system calls, and is compatible with a normal Linux RISC-V compiler.

## Example C API usage

Check out the [C API](/c/libriscv.h) and the [test project](/c/test/test.c).

## Handling instructions one by one

You can create your own custom instruction loop if you want to do things manually by yourself:

```C++
#include <libriscv/machine.hpp>
#include <libriscv/rv32i_instr.hpp>
...
Machine<RISCV64> machine{binary};
machine.setup_linux(
	{"myprogram"},
	{"LC_TYPE=C", "LC_ALL=C", "USER=root"});
machine.setup_linux_syscalls();

// Instruction limit is used to keep running
machine.set_max_instructions(50'000'000ull);

while (!machine.stopped()) {
	auto& cpu = machine.cpu;
	// Read next instruction
	const auto instruction = cpu.read_next_instruction();
	// Print the instruction to terminal
	printf("%s\n", cpu.to_string(instruction).c_str());
	// Execute instruction directly
	cpu.execute(instruction);
	// Increment PC to next instruction, and increment instruction counter
	cpu.increment_pc(instruction.length());
	machine.increment_counter(1);
}
```

## Executing a program in small increments

If we only want to run for a small amount of time and then leave the simulation, we can use the same example as above with an outer loop to keep it running as long as we want to until the machine stops normally.
```C++
	do {
		// Only execute 1000 instructions at a time
		machine.reset_instruction_counter();
		machine.set_max_instructions(1'000);

		while (!machine.stopped())
		{
			auto& cpu = machine.cpu;
			// Read next instruction
			const auto instruction = cpu.read_next_instruction();
			// Print the instruction to terminal
			printf("%s\n", cpu.to_string(instruction).c_str());
			// Execute instruction directly
			cpu.execute(instruction);
			// Increment PC to next instruction, and increment instruction counter
			cpu.increment_pc(instruction.length());
			machine.increment_counter(1);
		}

	} while (machine.instruction_limit_reached());
```
The function `machine.instruction_limit_reached()` only returns true when the instruction limit was reached, and not if the machine stops normally. Using that we can keep going until either the machine stops, or an exception is thrown.

## Setting up your own machine environment

You can create a machine without a binary, with no ELF loader invoked:

```C++
	Machine<RISCV32> machine;
	machine.setup_minimal_syscalls();

	std::vector<uint32_t> my_program {
		0x29a00513, //        li      a0,666
		0x05d00893, //        li      a7,93
		0x00000073, //        ecall
	};

	// Set main execute segment (12 instruction bytes)
	const uint32_t dst = 0x1000;
	machine.cpu.init_execute_area(my_program.data(), dst, 12);

	// Jump to the start instruction
	machine.cpu.jump(dst);

	// Geronimo!
	machine.simulate(1'000ull);
```

The fuzzing program does this, so have a look at that. There are also [unit tests](/tests/unit/micro.cpp).

## Adding your own instructions

See [this unit test](/tests/unit/custom.cpp) for an example on how to add your own instructions. They work in all simulation modes.

## Documentation

[Fast custom RISC-V compiler](docs/NEWLIB.md)

[System calls](docs/SYSCALLS.md)

[Freestanding environments](docs/FREESTANDING.md)

[Function calls into the VM](docs/VMCALL.md)

[Debugging with libriscv](docs/DEBUGGING.md)

[Example programs](/examples)

[Unit tests](/tests/unit)


## Dispatch modes

### Bytecode simulation modes

- Bytecode simulation using switch case
- Threaded bytecode simulation
- Tailcall bytecode simulation

This can be controlled with CMake options, however the default is usually fastest.

### Remote GDB using RSP server

Using an [RSP server](/lib/libriscv/rsp_server.hpp):

- Step through the code using GDB and your programs embedded pretty printers

### Build your own interpreter loop

- Using [CPU::step_one()](/lib/libriscv/cpu.hpp), one can step one instruction
- Precise simulation with custom conditions

### Using the DebugMachine wrapper

Using the [debugging wrapper](/lib/libriscv/debug.hpp):

- Simulate one instruction at a time
- Verbose instruction logging
- Debugger CLI with commands

### Binary translation

The binary translation feature (accessible by enabling the `RISCV_BINARY_TRANSLATION` CMake option) can greatly improve performance in some cases, but requires compiling the program on the first run. The RISC-V binary is scanned for code blocks that are safe to translate, and then a C compiler is invoked on the generated code. This step takes a long time. The resulting code is then dynamically loaded and ready to use. The feature is stable, but still undergoing work.

Instead of JIT, the emulator supports translating binaries to native code using any local C compiler. You can control compilation by passing CC and CFLAGS environment variables to the program that runs the emulator. You can show the compiler arguments using VERBOSE=1. Example: `CFLAGS=-O2 VERBOSE=1 ./myemulator`. You may use `KEEPCODE=1` to preserve the generated code output from the translator for inspection. `NO_TRANSLATE=1` can be used to disable binary translation in order to compare output or performance.

An experimental libtcc mode can be unlocked by enabling `RISCV_EXPERIMENTAL`, called `RISCV_LIBTCC`. When enabled, libriscv will invoke libtcc on code generated for each execute segment. It is usually faster than bytecode simulation, but not always.


## Experimental and special features

### Read-write arena

The read-write arena simplifies memory operations immediately outside of the loaded ELF, leaving the heap unprotectable. If page protections are needed, pages can still be allocated outside of the arena memory area, and there page protections will apply as normal.

### Multiprocessing

There is multiprocessing support, but it is in its early stages. It is achieved by calling a (C/SYSV ABI) function on many machines, with differing CPU IDs. The input data to be processed should exist beforehand. It is not well tested, and potential page table races are not well understood. That said, it passes manual testing and there is a unit test for the basic cases.

### Embedded libtcc

When binary translation is enabled, the experimental option `RISCV_LIBTCC` is available. libtcc will be embedded in the RISC-V emulator and used as compiler for binary translation. `libtcc-dev` package will be required for building.


## Game development example

Have a look at [RVScript](https://github.com/fwsGonzo/rvscript). It embeds libriscv in a tiny example framework and automatically builds fully functional C++ programs for low latency scripting.
