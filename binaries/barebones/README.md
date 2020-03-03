Barebones RISC-V binary
===========================

Has a tiny libc. Enough for simple optimized inline C++. `malloc` and `free` etc. is implemented as system calls for native performance.

To run this you will have to disable newlib/linux options in the emulator:
```
	static constexpr bool full_linux_guest = false;
	static constexpr bool newlib_mini_guest = false;
```
Which branches into this:
```
	setup_minimal_syscalls(state, machine);
	setup_native_heap_syscalls(state, machine);
```
It activates a non-standard system call implementation made for these tiny binaries. Perhaps one day it will be usable for sandboxed C++ program execution.

Have a look at `libc/heap.hpp` for the syscall numbers and calling.
