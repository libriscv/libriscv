Barebones RISC-V binary
===========================

To run this you will have to use the _rvmicro emulator_. It activates a non-standard system call implementation specially made for these tiny binaries. It is intended to be usable for sandboxed C/C++ program execution.

## Accelerated runtimes

Has a tiny libc mode. Enough for simple optimized C++. If you enable newlib mode you will get a full standard library, except threads. In both modes functions like `malloc`, `free`, `memcpy`, `memcmp`, `strlen` and many others are implemented as system calls for better performance. The tiny libc is a bit of a mess but it can easily be improved upon as the barebones C standard functions are easy to implement.

The linux64 program takes 11 milliseconds to complete on my machine. The barebones examples both take less than 2 milliseconds to complete, and they do a lot more work testing the multi-threading.

Have a look at `libc/heap.hpp` for the syscall numbers and calling.

## Tiny builds

A minimal 32-bit tiny libc build with MINIMAL, LTO and GCSECTIONS enabled yields a _9.5kB binary_ that uses 260kB memory.

A minimal 64-bit Newlib build with MINIMAL, LTO and GCSECTIONS enabled yields a _284kB binary_ that uses 196kB memory.

The reason for the difference in memory use is likely that newlib puts more things in .rodata which is heavily optimized in the emulator. Most rodata pages re-use the binary instead of duplicating the data, and it doesn't count towards the memory usage for various reasons. rodata is shared between all forks of the machine, and it makes sense to not count memory that is only required for the main VM.
