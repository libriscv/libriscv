Debugging with libriscv
================

Debugging with *libriscv* can be as complex or simplistic as one wants, depending on what you have to work with. If you don't have any symbols, and you have advanced knowledge of RISC-V you can step through the program instruction by instruction. This was the method used when developing the RISC-V emulator, but it's not always super helpful when debugging a normal mostly-working program in a robust emulator.

There are three main methods to debugging. One is using the built-in debugging facilities. Another is stepping through the program yourself manually, and checking for any conditions you are interested in, using the emulators state. And the third option is connecting with GDB remotely, which is what you are after if you are just debugging a normal program. Importantly, GDB gives you good introspection of the environment when using any modern language with debuginfo support.

## Debugging with the emulator itself

This method is platform-independent and works everywhere, but you will want to do it from a terminal so you can type commands. It allows you to step through the program instruction by instruction and trap on execute, reads and writes. Doing that, however, requires you to program this behavior. libriscv is fundamentally a library with a flexible, programmable emulator.

You can enable the RISCV_DEBUG CMake option if you want, but it is entirely optional. It will enable extra debugging features in the machine.

```C++
	DebugMachine debug { machine };
	// Print all instructions one by one
	debug.verbose_instructions = true;
	// Break immediately
	debug.print_and_pause();

	try {
		debug.simulate();
	} catch (riscv::MachineException& me) {
		printf(">>> Machine exception %d: %s (data: 0x%lX)\n",
				me.type(), me.what(), long(me.data()));
		debug.print_and_pause();
	} catch (std::exception& e) {
		printf(">>> General exception: %s\n", e.what());
		debug.print_and_pause();
	}
```
An example of how to use the built-in CLI to step through instruction by instruction.

## Debugging manually with libriscv

By simulating a single instruction using `CPU::step_one()` we can programmatically apply any conditions we want:

```C++
DebugMachine debug { machine };

debug.simulate([] (auto& debug) {
	if (debug.machine.cpu.reg(10) == 0x1234)
		debug.print_and_pause();
});

```
This will step through the code one instruction at a time until register A0 is 0x1234, then break into the debugging CLI and print location and registers.

## Debugging remotely with GDB

To get a GDB capable of debugging RISC-V you will need to install your distros equivalent of `gdb-multiarch`. It will have RISC-V support built in, and it will detect the architecture of ELF programs you load in it.

```
sudo apt install gdb-multiarch
```

With the CLI, start the program with `--gdb` and connect from another terminal:

```
$ ./rvlinux --gdb my.elf
GDB server is listening on localhost:2159

$ gdb-multiarch -ex "target remote :2159"
```

The CLI provides the executable path so GDB can load its symbols automatically. You can also specify the executable directly: `gdb-multiarch my.elf`. The program starts paused at its entry point. Use `break main` and `continue` to begin debugging; `run` and `start` are not available on a remote target.

Remember to build your RISC-V program with `-ggdb3 -O0`, otherwise you will not get complete information during the debugging session.

Supported features:

- Unlimited breakpoints, conditional breakpoints, `tbreak`, `until`, `advance`, `finish`, `next`, `step`, `stepi` and `catch throw`.
- Watchpoints: `watch` (write), `rwatch` (read) and `awatch` (access). Write watchpoints check for changes to the watched memory after each instruction, including changes made by system calls. Read and access watchpoints decode scalar loads, stores and atomics, but do not support vector memory operations.
- Backtraces and function calls within the program (`print func(1, 2)`).
- All registers: integer, floating-point (`$ft0`, `$fcsr`), vector (`$v0`, `$vl`, `$vlenb`, etc.) and the `$cycle` and `$instret` counters, both of which report the instruction counter.
- Threads (when POSIX threads are enabled): `info threads`, `thread N`, `thread apply all bt`.
- Ctrl-C interrupts a running program.
- CPU exceptions stop the program with the corresponding signal: SIGSEGV for protection faults, SIGILL for illegal instructions, SIGBUS for misaligned access, and so on. The PC points to the faulting instruction, and the exception message appears in the GDB console.
- An `ebreak` instruction compiled into the program stops execution with SIGTRAP. The PC points to the instruction after the `ebreak`, allowing execution to resume.
- Program termination reports the exit status. `kill` stops the machine; `detach` or quitting GDB allows the program to continue independently.
- Static PIE programs: GDB receives the auxiliary vector needed to relocate symbols.

While GDB is connected, the program executes one instruction at a time at roughly 200 million instructions per second. This provides precise debugging but is slower than normal execution. The decoder cache is bypassed, so functions patched through the cache (e.g., by `--libc-fastpath`) execute their original code under the debugger.

For dynamically linked programs, the CLI loads the dynamic linker as the main executable, and GDB treats it as such. GDB finds the shared libraries but does not automatically locate the program's own symbols. Add them manually with `add-symbol-file`. Prefer static linking when debugging.

### Embedding the GDB server

```C++
#include <libriscv/rsp_server.hpp>
...
template <int W>
void gdb_listen(uint16_t port, Machine<W>& machine)
{
	printf("GDB server is listening on localhost:%u\n", port);
	riscv::RSP<W> server { machine, port };
	auto client = server.accept();
	if (client != nullptr) {
		printf("GDB connected\n");
		// Optional: Provide the executable path so GDB can load its symbols
		client->set_exec_file("/path/to/program.elf");
		while (client->process_one());
	}
	// Finish the *remainder* of the program
	if (!machine.stopped())
		machine.simulate(/* machine.max_instructions() */);
}
```

To debug a single function call, prepare the call before connecting GDB. When the function returns, GDB reports program termination with the function's return value as the exit status:

```C++
	machine.setup_call(6);
	machine.cpu.jump(machine.address_of("fib"));
	gdb_listen(2159, machine);
	// The return value is in A0
```

Other `RSPClient` options:

- `set_instruction_limit(n)`: Stop with SIGXCPU after executing `n` instructions during a single `continue` command. No limit is set by default; use Ctrl-C to interrupt execution.
- `on_stopped(callback)`: Invoke the callback when the machine stops (e.g., when the program exits or `machine.stop()` is called), replacing the default exit report to GDB.
- `set_debug_printer(printer)`: Send exception messages to the printer as well as the GDB console.
- `set_verbose(true)`: Print every packet sent and received.

## Debugging remotely using program breakpoints

One powerful option is opening up for a remote debugger on-demand. To do this you need to implement a system call that simply does what the previous section does: Opening up a port for a remote debugger. The difference is that you do it during the system call, so that you can debug things like failed assertions and other should-not-get-here things. You can open up a debugger under any condition. GDB will resume from where the program stopped.

In other words, call the `gdb_listen` function above during anytime you want to have a look at what's going on.

The most likely system call candidate for this behavior is for handling EBREAK interruptions. The emulator defines `RISCV_SYSCALL_EBREAK_NR` by default to `RISCV_SYSCALLS_MAX-1`, but it can be overridden. Reaching EBREAK is always handled as a system call in the emulator.

To avoid having to repeat yourself, create a GDB script to automatically connect and enter TUI mode:
```
target remote :2159
layout next
```
Then run `gdb -x myscript.gdb`.

Good luck!
