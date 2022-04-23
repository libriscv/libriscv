# System Calls

System calls are services that the program running inside the emulator can request from the host to function properly, and be able to do useful things other than just calculations. For example, the only way to be able to print text in *your* terminal from inside the virtual machine is to request the system to do that, and then hope that it does! The host system is under no obligation to do anything, especially if it doesn't seem like a good idea to do!

If you use `printf("Hello world!\n");` in your program, it will likely cause a call to the `write` system call wrapper. This wrapper then does the actual system call itself, which boils down to setting up the arguments and then executing an `ECALL` instruction. At that point the emulator itself will stop executing the virtual machine (the guest) and instead handle this system call. When it's done handling the system call it will continue running the virtual machine. The return value of the system call usually indicates whether or not it succeeded or not.

## System call numbers

The numbers are taken from `linux-headers/include/asm-generic/unistd.h` in the riscv-gnu-toolchain, which are ultimately Linux system call numbers. You can also make up your own system calls, and even how to do a system call (the ABI). [List of system call numbers](https://github.com/riscv-collab/riscv-gnu-toolchain/blob/master/linux-headers/include/asm-generic/unistd.h).

Example:
```
#define __NR_getcwd 17
```
[getcwd()](http://man7.org/linux/man-pages/man2/getcwd.2.html) is system call number 17, and returns the current working directory. You can also run `man getcwd` to get the same page up in a terminal.

## Standard library system calls

When running a RISC-V program in the emulator, you can see messages about unhandled system calls as long as you provide a callback function to the machine by setting `machine.on_unhandled_syscall = my_function;`. Unhandled system calls return `-ENOSYS` by default. To implement missing system calls, you have to set a handler for it.

These are system calls executed by the C and C++ standard libraries, and some of them are not optional. For example, there is no graceful way to shutdown the program without implementing `exit` (93) or `exit_group` (94).

If you want to see the stdout output from your hello world, you will also want to implement either `write` or `writev` (depending on the C library).

## Handling a system call

Let's start with an example of handing exit:
```C++
template <int W>
void syscall_exit(Machine<W>& machine)
{
	// Get the first argument as an int
	auto [exit_code] = machine.template sysargs <int> ();
	// Do something with exit_code
	printf("The machine exited with code: %d\n", exit_code);
	// The exit system call makes the machine stop running
	machine.stop();
}
```
Our exit system call handler extracts the exit status code from the first argument to the system call, stops the machine and then returns a mandatory value. The return value in this system call is not important, as the machine has already been stopped and isn't expecting to return from the system call it just ran.

Stopping the machine in practice just means exiting the `Machine::simulate()` loop.

Be careful about modifying registers during system calls, as it may cause problems
in the simulated program. The program may only be expecting you to modify register A0.

## Handling an advanced system call

If we want stdout from the VM printed in our terminal, we should handle `write`:

```C++
#include <unistd.h>

template <int W>
void syscall_write(Machine<W>& machine)
{
	const auto [fd, address, len] =
		machine.template sysargs <int, address_type<W>, address_type<W>> ();
	// We only accept standard output pipes, for now :)
	if (fd == 1 || fd == 2) {
		char buffer[1024];
		const size_t len_g = std::min(sizeof(buffer), len);
		machine.memory.memcpy_out(buffer, address, len_g);
		// Write buffer to our terminal!
		machine.set_result_or_error(write(fd, buffer, len_g));
		return;
	}
	machine.set_result(-EBADF);
}
```
Here we extract 3 arguments, `int fd, void* buffer, size_t len`, looks familiar? We have to make sure fd is one of the known standard pipes, otherwise the VM could start writing to real files open in the host process!

The return value of a call into a kernel is usually a success or error indication, and the way to set an error is to negate a POSIX error code. Success is often 0, however in this case the return value is the bytes written. To make sure we pass on errno properly, we use the helper function `machine.set_result_or_error()`. It takes care of handling the common error case for us.

## Installing system calls

To be able to handle system calls, they need to be installed into the machine:

```
const int SYS_WRITE = 64;
machine.install_syscall_handler(SYS_WRITE, syscall_write<W>);
```
Now when the VM guest calls `printf()` and the C library uses the `write` syscall, our `syscall_write` function will be called.

This method of installing your own system call handlers effectively means you can curate an API for your particular needs.

## Communicating the other way

While the example above handles a copy from the guest- to the host-system, the other way around is the best way to handle queries. For example, the `getcwd()` function requires passing a buffer and a length:

```C++
// this is normal C++
std::array<char, PATH_MAX> buffer;
char* b = getcwd(buffer.data(), buffer.size());
assert(b != nullptr);

printf("The current working directory is: %s\n", buffer.data());
```

To handle this system call, we will need to copy into the guest:

```C++
#include <unistd.h>

template <int W>
void syscall_getcwd(Machine<W>& machine)
{
	const auto [address, len] =
		machine.template sysargs <address_type<W>, address_type<W>> ();
	// make something up! :)
	const char path[] = "/home/vmguest";
	// we only accept lengths of at least sizeof(path)
	if (len >= sizeof(path)) {
		machine.copy_to_guest(address, path, sizeof(path));
		machine.set_result(address);
		return; // ^ this way will copy the terminating zero as well!
	}
	// for unacceptable values we return null
	machine.set_result(0);
}
```

If in doubt, just use `address_type<W>` for the syscall argument, and it will be the same size as a register, which all system call arguments are anyway.

## The RISC-V system call ABI

On RISC-V a system call has its own instruction: `ECALL`. A system call can have up to 7 arguments and has 1 return value. The arguments are in registers A0-A6, in that order, and the return value is written into A0 before giving back control to the guest. A7 contains the system call number. These are all integer/pointer registers.

To pass larger data around, the guest should allocate buffers of the appropriate size and pass the address of the buffers as arguments to the system call.

## Special note on EBREAK

The `EBREAK` instruction is handled as a system call in this emulator, specifically it uses the system call number `riscv::SYSCALL_EBREAK`, which at the time of writing is put at the end of the system call table (N-1), however it can be changed in the common header or by setting a global define `RISCV_SYSCALL_EBREAK_NR` to a specific number.

`EBREAK` is very convenient for debugging purposes, as adding it somewhere in the code is very simple: `asm("ebreak");`.

NOTE: Be careful of `__builtin_trap()`, as it erroneously assumes that you are never returning, and the compiler will stop producing instructions after it.
