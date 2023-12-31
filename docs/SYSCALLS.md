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
Our exit system call handler extracts the exit status code from the first argument to the system call, prints it to stdout and then stops the machine. Installing the system call handler in a machine is straight-forward:

```C++
machine.install_syscall_handler(93, syscall_exit<RISCV64>);
machine.install_syscall_handler(94, machine.syscall_handlers.at(93));
```
Here we installed a 64-bit system call handler for both `exit` (93) and `exit_group` (94).

```C++
Machine<RISCV64>::install_syscall_handler(93, syscall_exit<RISCV64>);
```
System call handlers are static by design to avoid system call setup overhead when creating machines.

Stopping the machine in practice just means exiting the `Machine::simulate()` loop.

Be careful about modifying registers during system calls, as it may cause problems
in the simulated program. The program may only be expecting you to modify register A0 (return value) or modifying memory pointed to by a system call argument.

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

## Zero-copy write

`write` can be implemented in a zero-copy manner:

```C++
#include <unistd.h>

template <int W>
void syscall_write(Machine<W>& machine)
{
	const auto [fd, address, len] =
		machine.template sysargs <int, address_type<W>, address_type<W>> ();
	// We only accept standard output pipes, for now :)
	if (fd == 1 || fd == 2) {
		// Zero-copy buffers pointing into guest memory
		riscv::vBuffer buffers[16];
		size_t cnt =
			machine.memory.gather_buffers_from_range(16, buffers, address, len);
		// We could use writev here, but we will just print it instead
		for (size_t i = 0; i < cnt; i++) {
			machine.print(buffers[i].ptr, buffers[i].len);
		}
		machine.set_result(len);
		return;
	}
	machine.set_result(-EBADF);
}
```
`gather_buffers_from_range` will fill an iovec-like array of structs up until the given number of buffers. We can then use that array to print or forward the data without copying anything.

`gather_buffers_from_range` will concatenate sequential parts of guest memory, and very often even just 16 gather-buffers are enough to cover ~99% of cases. This is especially the case if the read-write arena is enabled. One should not think of a single buffer as page-sized, but rather sequential memory up until the next buffer.

## Memory helper methods

A fictive system call that has a single string as system call argument can be implemented in a variety of ways:

```C++
template <int W>
void syscall_string(Machine<W>& machine)
{
	// 1. Read the guests virtual address and length
	const auto [address, len] =
		machine.template sysargs <address_type<W>, address_type<W>> ();

	// Create a buffer and copy into it. Page protections apply.
	std::vector<uint8_t> buffer(len);
	machine.memory.copy_from_guest(buffer.data(), address, len);

	// 2. Read-only iovec buffers for readv/writev etc. Page protections apply.
	riscv::vBuffer buffers[16];
	size_t cnt =
		machine.memory.gather_buffers_from_range(16, buffers, address, len);
	const ssize_t res =
		writev(1, (struct iovec *)&buffers[0], cnt);

	// 2. Directly read a zero-terminated string. Page protections apply.
	const auto string = machine.memory.memstring(address);

	// Shortcut:
	const auto [string] =
		machine.template sysargs <std::string> (); // Consumes 1 register

	// 3. Get a string view directly (fastest option, read-write arena only)
	// Page protections do not apply inside the read-write arena. It is always
	// read-write, but also always sequential.
	const auto strview = machine.memory.rvview(address, len);

	// Shortcut:
	const auto [string] =
		machine.template sysargs <std::string_view> (); // Consumes 2 registers

	// 4. Get a zero-copy buffer. Page protections apply.
	const auto rvbuffer = machine.memory.rvbuffer(address, len);

	std::string str = rvbuffer.to_string();

	std::vector<uint8_t> buf;
	rvbuffer.copy_to(buf);

	if (buffer.is_sequential()) {
		consumer(buffer.data(), buffer.size());
	}

	// Shortcut:
	const auto [string] =
		machine.template sysargs <riscv::Buffer> (); // Consumes 2 registers
}
```

These are various ways that you can safely look at the guests memory provided by system call arguments. Most of these have a third argument setting a maximum length that puts a limit on the size of the buffers length. For example the maxlen argument in `memory.memstring(addr, len, maxlen)`.

One last function that needs to be mentioned is how to get writable buffers into the guest for writing eg. from a iovec-like reader in the host:

```C++
	riscv::vBuffer buffers[16];
	size_t cnt =
		machine.memory.gather_writable_buffers_from_range(16, buffers, address, len);
	const ssize_t res =
		readv(1, (struct iovec *)&buffers[0], cnt);
```

Using `gather_writable_buffers_from_range` we can let the Linux kernel block and read into the guests memory until completion.

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

For 32-bit, every 64-bit integer argument will use 2 registers. For example, the system call `uint64_t my_syscall(uint64_t a, uint64_t b, uint64_t c)` would use 6 integer registers for its arguments (A0-A5), and 2 return registers (A0, A1).

Floating-point arguments can be in FA0-FA7, however they are rarely (if ever) used for system calls.

To pass larger data around, the guest should allocate buffers of the appropriate size and pass the address of the buffers as arguments to the system call.

## Special note on EBREAK

The `EBREAK` instruction is handled as a system call in this emulator, specifically it uses the system call number `riscv::SYSCALL_EBREAK`, which at the time of writing is put at the end of the system call table (N-1), however it can be changed in the common header or by setting a global define `RISCV_SYSCALL_EBREAK_NR` to a specific number.

`EBREAK` is very convenient for debugging purposes, as adding it somewhere in the code is very simple: `asm("ebreak");`.

NOTE: Be careful of `__builtin_trap()`, as it erroneously assumes that you are never returning, and the compiler will stop producing instructions after it.
