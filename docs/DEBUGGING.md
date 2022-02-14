Debugging with libriscv
================

Debugging with *libriscv* can be as complex or simplistic as one wants, depending on what you have to work with. If you don't have any symbols, and you have rich knowledge of RISC-V you can enable the RISCV_DEBUG CMake option, and step through the program instruction by instruction. This was the method used when developing the RISC-V emulator, but it's not very helpful when debugging a normal mostly-working program in a robust emulator.

There are three main methods to debugging. One is using the built-in debugging facilities enabled with RISCV_DEBUG. Another is stepping through the program yourself manually, and checking for any conditions you are interested in, using the emulators state. And the third option is connecting with GDB remotely, which is what you are after if you are just debugging a normal program. Importantly, GDB gives you good introspection of the environment when using any modern language with debuginfo support.

## Debugging with the emulator itself

This method is platform-independent and works everywhere, but you will want to do it from a terminal so you can type commands. It allows you to step through the program instruction by instruction and trap on execute, reads and writes. Doing that, however, requires you to program this behavior. libriscv is fundamentally a library with a flexible, programmable emulator.

Start by enabling the RISCV_DEBUG CMake option. This will enable several debugging options in the machine, as well as give you access to a debugging CLI.

```C++
	// Print all instructions one by one
	machine.verbose_instructions = true;
	// Break immediately
	machine.print_and_pause();

	try {
		machine.simulate();
	} catch (riscv::MachineException& me) {
		printf(">>> Machine exception %d: %s (data: 0x%lX)\n",
				me.type(), me.what(), me.data());
		machine.print_and_pause();
	} catch (std::exception& e) {
		printf(">>> Exception: %s\n", e.what());
		machine.print_and_pause();
	}
```
An example of how to use the built-in CLI to step through instruction by instruction.

## Debugging manually with libriscv

By simulating a single instruction using `CPU::step_one()` we can programmatically apply any conditions we want:

```C++
machine.set_max_instructions(16'000'000);
while (!machine.stopped()) {
    machine.cpu.step_one();
    if (machine.cpu.reg(10) == 0x1234) machine.print_and_pause();
}
```
This will step through the code until register A0 is 0x1234, then break into the debugging CLI (which is enabled with RISCV_DEBUG). You may not want to enable RISCV_DEBUG at all, in which case replace the call to print_and_pause with your own handling.

Setting the max instructions will prevent the machine from appearing stopped. There is always an instruction limit where the loop will end, however you can greatly increase it if you don't want to exit the loop prematurely. If an exit system call is encountered it typically calls `Machine::stop()`, which will set max instructions to 0, exiting the loop. This is not mandated behavior as the exit function is a syscall callback function, and the loop is controlled by you (the user). In short, there are many ways to stop a machine, but the default method is to just check if `instruction counter >= max instructions`.

## Debugging remotely with GDB

To get a GDB capable of debugging RISC-V you will need to download and build the [RISC-V toolchain](https://github.com/riscv-collab/riscv-gnu-toolchain). Building GDB is always enabled when configuring. There is no difference between the 32- and 64-bit versions, afaik. On Windows you can get access to riscvXX-...-gdb using WSL2.

Once you have a RISC-V aware GDB you can `target remote localhost:2159` to connect when the emulator is waiting for a debugger.

```C++
#include <libriscv/rsp_server.hpp>
...
template <int W>
void gdb_listen(uint16_t port, Machine<W>& machine)
{
	riscv::RSP<W> server { machine, port };
	auto client = server.accept();
	if (client != nullptr) {
		printf("GDB connected\n");
		while (client->process_one());
	}
	// Finish the *remainder* of the program
	machine.simulate();
}
```

Remember to build your RISC-V program with `-ggdb3 -O0`, otherwise you will not get complete information during the debugging session.

You don't have to rebuild the emulator to enable this kind of debugging. But you may want to have an environment variable that disables binary translation if you are using that, because it compacts a block of code into "a single instruction," which can cause you to skip over lots of code, and the emulator won't be able to pause in the middle of that, because it's all become one unit of code. So remember to disable binary translation when debugging. Binary translation can be enabled and disabled using a run-time parameter: Setting `MachineOptions::translate_blocks_max` to 0 will disable it.

The RSP client will automatically feed new instruction limits to the machine when you type continue in GDB. This is to prevent you from being unable to continue running the code. Additionally, instruction counting may not be reliable if you debug a program and then try to use it for other purposes after. One can jump around in the code with GDB, which will continually increase the counter. A solution can be to store the counter values before debugging and then restoring the values after.

## Debugging remotely using program breakpoints

One powerful option is opening up for a remote debugger on-demand. To do this you need to implement a system call that simply does what the previous section does: Opening up a port for a remote debugger. The difference is that you do it during the system call, so that you can debug things like failed assertions and other should-not-get-here things. You can open up a debugger under any condition. GDB will resume from where the program stopped.

In other words, call the `gdb_listen` function above during anytime you want to have a look at what's going on.

The most likely system call candidate for this behavior is for handling EBREAK interruptions. The emulator defines `RISCV_SYSCALL_EBREAK_NR` by default to `RISCV_SYSCALLS_MAX-1`, but it can be overridden. Reaching EBREAK is always handled as a system call in the emulator.

To avoid having to repeat yourself, create a GDB script to automatically connect and enter TUI mode:
```
target remote localhost:2159
layout next
```
Then run `gdb -x myscript.gdb`.

Good luck!
