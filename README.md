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

Building and running your own ELF files that can run in freestanding RV32GC is quite challenging, so consult the `barebones` example! It's a bit like booting on bare metal using multiboot, except you have easier access to system functions. The fun part is of course the extremely small binaries and total control over the environment.

The `newlib` example project have much more C and C++ support, but still misses things like environment variables and such. This is a deliberate design as newlib is intended for embedded development. It supports C++ RTTI and exceptions, and is the best middle-ground for running a fuller C++ environment that still produces small binaries.

Finally, the `full` example project uses the Linux-configured cross compiler and will expect you to implement quite a few system calls just to get into `int main()`. In addition, you will have to setup argv, env and the aux-vector. There is a helper method to do this in the src folder. If you want to implement threads to really get to all the fun stuff (like the coming C++20 coroutines), then this is where you can do that, as newlib simply won't enable threads.

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

You can find details on the Linux system call ABI online as well as in the `syscalls.hpp` header in the src folder. You can use this header to make syscalls from your RISC-V programs. It is the Linux RISC-V syscall ABI.

Be careful about modifying registers during system calls, as it may cause problems
in the simulated program.

## Creating your own environment

If you want a completely freestanding environment in your embedded program you will need to do a few things in order to call into a C function properly and use both stack and static storage.

Make sure your stack is aligned to the mandatory alignment of your architecture, which in the case for RISC-V is 16-bytes. The first instruction we are going to execute is `auipc` though, so maybe we only need 4- or 8-byte alignment, but we will not play with fire. Let's align the stack pointer from outside the machine:

```C++
auto& sp = machine.cpu.reg(RISCV::REG_SP);
sp &= ~0xF; // mandated 16-byte stack alignment
```

Also, perhaps you want to avoid using a low address as the initial stack value, as it could mean that some stack pointer value will be 0x0 at some point, which could mysteriously fail one of your own checks or asserts. Additionally, the machine automatically makes the zero-page unreadable on start to help you catch accesses to the zero-page, which are usually bugs. It's fine to start the stack at 0x0 though, as the address will wrap around and start pushing bytes at the top of the address space, at least in 32-bit.

From now on, all the example code is going to be implemented inside the guest binary, in the startup function which is always named `_start` and is a C function. In C++ you would have to write it like this:

```C++
extern "C"
void _start()
{
	// startup code here
}
```

Second, you must set the GP pointer to the absolute address of `__global_pointer`. The only way to do that is to disable relaxation:

```C++
asm volatile
("   .option push 				\t\n\
	 .option norelax 			\t\n\
	 1:auipc gp, %pcrel_hi(__global_pointer$) \t\n\
	 addi  gp, gp, %pcrel_lo(1b) \t\n\
	.option pop					\t\n\
");
// make sure all accesses to static memory happen after:
asm volatile("" ::: "memory");
```

If the GP register is not initialized properly, you will not be able to use static memory, and you will get all sorts of weird problems.

Third, clear .bss which is the area of memory used by zero-initialized variables:
```C++
extern char __bss_start;
extern char __BSS_END__;
for (char* bss = &__bss_start; bss < &__BSS_END__; bss++) {
	*bss = 0;
}
```

After this you might want to initialize your heap, if you have one. If not, consider getting a tiny heap implementation from an open source project. Perhaps also initialize some early standard out (stdout) facility so that you can get feedback from subsystems that print errors during initialization.

Next up is calling global constructors, which while not common in C is very common in C++, and doesn't contribute much to the binary size anyway:

```C++
extern void(*__init_array_start [])();
extern void(*__init_array_end [])();
int count = __init_array_end - __init_array_start;
for (int i = 0; i < count; i++) {
	__init_array_start[i]();
}
```
Now you are done initializing the absolute minimal C runtime environment. Calling main is as simple as:

```C++
extern int main(int, char**);

// geronimo!
_exit(main(0, nullptr));
```

Here we mandate that you must implement `int main()` or get an undefined reference, and also the almost-mandatory `_exit` system call wrapper. You can implement `_exit` like this:

```C++
#define SYSCALL_EXIT   93

extern "C" {
	__attribute__((noreturn))
	void _exit(int status) {
		syscall(SYSCALL_EXIT, status);
		__builtin_unreachable();
	}
}
```

You will need to handle the EXIT system call on the outside of the machine as well, to stop the machine. If you don't handle the EXIT system call and stop the machine, it will continue executing instructions past the function, which does not return. It will cause problems. A one-argument system call can be implemented like this:

```C++
template <int W>
long syscall_exit(riscv::Machine<W>& machine)
{
	const int status = machine.template sysarg<int> (0);
	printf(">>> Program exited, exit code = %d\n", status);
	machine.stop();
	return status;
}
```
And installed as a 32-bit system call handler like this:
```C++
machine.install_syscall_handler(93, syscall_exit<riscv::RISCV32>);
```

Since all system calls have to return, we might as well return the status code back, and the only reason we are doing this is because it lets us abuse `_exit` to make function calls into the environment. Otherwise, the return value has no meaning here because we already stopped running the machine. The machine instruction processing loop will stop running immediately after this system call has been invoked.

And finally, to make a system call with one (1) argument from the guest environment you could do something like this (in C++):
```C++
inline long syscall(long n, long arg0)
{
	register long a0 asm("a0") = arg0;
	register long syscall_id asm("a7") = n;

	asm volatile ("scall" : "+r"(a0) : "r"(syscall_id));

	return a0;
}
```
All integer and pointer arguments are in the a0 to a6 registers, which adds up to 7 arguments in total. The return value of the system call is written back into a0.

If you have done all this you should now have the absolute minimum C and C++ freestanding environment up and running. Have fun!


## Calling into the VM environment

It's possible to make function calls into the environment, but there are a handful of things that need to be taken care of in order to do that. First off, you should not return from `int main()`, as that will call global destructors which will make the C run-time environment unreliable. Instead, one should call `_exit(status)` from `int main()`, which immediately exits the machine, but does not call global destructors and free resources.

It is not unsafe to call `_exit()` or any other equivalent function directly, as all it does is invoke the EXIT system call, which immediately stops execution of the program. In a normal operating system this also makes the execution environment (usually a process) disappear, and releases all the resources back. In this case we just want to preserve the machine state (which is in a good known state) while also stopping execution, so that we can call into the programs functions directly.

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
