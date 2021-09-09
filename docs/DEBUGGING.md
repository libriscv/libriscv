Debugging with libriscv
================

Debugging with libriscv can be as complex or simplistic as one wants, depending on what you have to work with. If you don't have any symbols, and you have rich knowledge of RISC-V you can enable the RISCV_DEBUG CMake option, and step through the program instruction by instruction. This was the method used when developing the RISC-V emulator, but it's not very helpful when debugging a normal mostly-working program in a robust emulator.

So, there are three methods to debugging. One is using the built-in debugging facilities enabled with RISCV_DEBUG. Another is stepping through the program yourself manually, and checking for any conditions you are interested in, using the emulators state. And the third option is connecting with GDB remotely, which is what you are after if you are just debugging a normal program.

## Debugging with the emulator itself

This method is platform-independent and works everywhere. It allows you to step through the program instruction by instruction and trap on execute, reads and writes. Doing that, however, requires you to program this behavior. libriscv is fundamentally a library with a flexible, programmable emulator.

Start by enabling the RISCV_DEBUG option. This will enable several debugging options in the machine, as well as give you access to a debugging CLI.

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
while (!machine.stopped()) {
    machine.cpu.step_one();
    if (machine.cpu.reg(10) == 0x1234) machine.print_and_pause();
}
```
This will step through the code until register A0 is 0x1234, then break into the debugging CLI (which is enabled with RISCV_DEBUG).

## Debugging remotely with GDB

To get a GDB capable of debugging RISC-V you will need to download and build the RISC-V toolchain. It's always enabled. There is no difference between the 32- and 64-bit versions. On Windows you can get access to riscvXX-...-gdb using WSL/WSL2.

Once you have GDB, you can `target remote localhost:2159` to connect when the emulator is waiting for a debugger.

```C++
#include <libriscv/rsp_server.hpp>
...
void MyClass::gdb_listen(uint16_t port)
{
	riscv::RSP<W> server { machine, port };
	auto client = server.accept();
	if (client != nullptr) {
		printf("GDB connected\n");
		while (client->process_one());
	}
	// If the machine has not stopped running here, finish the program
}
```

Remember to build your RISC-V program with `-ggdb3 -O0`, otherwise you will not get complete information during the debugging session.

You don't have to rebuild your engine to enable this kind of debugging. But you may want to have an environment variable that disables binary translation if you are using that, because it compacts a block of code into "a single instruction," which can cause you to skip over lots of code, and the emulator won't be able to pause in the middle of that, because it's all become one unit of code. So remember to disable binary translation when debugging.


## Debugging remotely using program breakpoints

One powerful option is opening up for a remote debugger on-demand. To do this you need to implement a system call that simply does what the previous section does: Opening up a port for a remote debugger. The difference is that you do it during the system call, so that you can debug things like failed assertions and other should-not-get-here things. You can open up a debugger under any condition. GDB will resume from where the program stopped.

In other words, call the `gdb_listen` function above during anytime you want to have a look at what's going on.

To avoid having to repeat yourself, create a GDB script to automtically connect and enter TUI mode:
```
target remote localhost:2159
layout next
```
Then run `gdb -x myscript.gdb`.

Good luck!
