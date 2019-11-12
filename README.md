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

You can find details on the Linux system call ABI online as well as in the `syscall.h`
header in the src folder. You can use this header to make syscalls from your RISC-V programs.
It is the Linux RISC-V syscall ABI.

Be careful about modifying registers during system calls, as it may cause problems
in the simulated program.

## Calling into the VM environment

It's possible to make function calls into the environment, but there are a handful of things that need to be taken care of in order to do that. First off, you should not return from `int main()`, as that will call global destructors which will make the
C run-time environment unreliable. Instead, one should call `_exit(status)` from `int main()`, which correctly exits the machine, but does not call global destructors.

Once the machine is no longer running, but still left in a state in which we can call into it, we have to make sure that our callable function in the VM is present in the symbol table of the ELF, so that we can find the address of this function. In addition, `_exit` must also be present in the ELF symbol tables, as it will be used as a way to exit the VM call and also optionally return a status code. This is done via hooking up the exit system call behind the scenes and extracting the exit status code after execution stops.

A third, and final, stumbling block is sometimes having functions, even those marked with `__attribute__((used))`, not appearing in the ELF symbol table, which is a linker issue. This can happen when using `-gc-sections` and friends. You can test if your symbol is visible to the emulator by using `machine.memory.resolve_address("myFunction")` which returns a memory address. If the address is 0, then the name was not found in the symbol table, which makes `vmcall(...)` impossible to perform.

Start by running the machine normally and complete `int main()` to make sure global constructors are called and the C run-time environment is fully initialized. So, if you are calling `_exit(0)` from main instead of returning, and not stripping ELF symbols from your binary you are ready to make function calls into the virtual machine.

Example which calls the function `test` with the arguments `555` and `666`:
```C++
	int ret = machine.vmcall("test", {555, 666});
	printf("test returned %d\n", ret);
```
Arguments are passed as a C++ initializer list of register-sized integers.

Instruction counters and registers are not reset on calling functions, so make sure to take that into consideration when measuring.

It is not recommended to copy data into guest memory and then pass pointers to this data to VM calls, as it's a very complex task to determine which memory is unused by the guest before and during the call. Instead, the guest can allocate room for the struct on its own, and then simply perform a system call, passing a pointer to the struct as an argument.


## Why a RISC-V library

It's a drop-in sandbox. Perhaps you want someone to be able to execute C/C++ code on a website, safely?

See the `webapi` folder for an example web-server that compiles and runs limited C/C++ code in a relatively safe manner. Ping me or create a PR if you notice something is exploitable.

Note that the web API demo uses a docker container to build RISC-V binaries, for security reasons. You can build the container with `docker build -t gcc9-rv32imac .` from the gcc folder. There is a test-script to see that it works called `dbuild.sh` which takes an input code file and output binary as parameters.
