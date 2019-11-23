# Function calls into the guest VM

## Requisites

It's possible to make function calls into the virtual machine, but there are a handful of things that need to be taken care of in order to do that. First off, you should not return from `int main()`, as that will call global destructors which will make the C run-time environment unreliable. Instead, one should call `_exit(status)` from `int main()`, which immediately exits the machine, but does not call global destructors and free resources.

It is not unsafe to call `_exit()` or any other equivalent function directly, as all it does is invoke the EXIT system call, which immediately stops execution of the program. In a normal operating system this also makes the execution environment (usually a process) disappear, and releases all the resources back. In this case we just want to preserve the machine state (which is in a good known state) while also stopping execution, so that we can call into the programs functions directly.

Once the machine is no longer running, but still left in a state in which we can call into it, we have to make sure that our callable function in the VM is present in the symbol table of the ELF, so that we can find the address of this function. In addition, `_exit` must also be present in the ELF symbol tables, as it will be used as a way to exit the VM call and also optionally return a status code. This is done via hooking up the exit system call behind the scenes and extracting the exit status code after execution stops.

A third, and final, stumbling block is sometimes having functions, even those marked with `__attribute__((used))`, not appearing in the ELF symbol table, which is a linker issue. This can happen when using `-gc-sections` and friends. You can test if your symbol is visible to the emulator by using `machine.memory.resolve_address("myFunction")` which returns a memory address. If the address is 0, then the name was not found in the symbol table, which makes `vmcall(...)` impossible to perform.

Start by running the machine normally and complete `int main()` to make sure global constructors are called and the C run-time environment is fully initialized. So, if you are calling `_exit(0)` from main instead of returning, and not stripping ELF symbols from your binary you are ready to make function calls into the virtual machine.

## Calling into the guest virtual machine

Example which calls the function `test` with the arguments `555` and `666`:
```C++
	int ret = machine.vmcall("test", {555, 666});
	printf("test returned %d\n", ret);
```


Arguments are passed as a C++ vector of register-sized integers.

Instruction counters and registers are not reset on calling functions, so make sure to take that into consideration when measuring.

It is not recommended to copy data into guest memory and then pass pointers to this data as arguments, as it's a very complex task to determine which memory is unused by the guest before and even during the call. Instead, the guest can allocate room for the struct on its own, and then simply perform a system call where it passes a pointer to the struct as an argument.


## Doing work in-between machine execution

Without using threads the machine program will simply run until it's completed, an exception occurs, or the machine is stopped from the outside during a trap or system call. It would be nice to have the ability to run the host-side program in-between without preemption. We can do this by making vmcall not execute machine instructions, and instead do it ourselves manually:

```
// Make a function call into the guest VM, but don't start execution
machine.vmcall("test", {555}, false);
// Run the program for X amount of instructions, then print something, then
// resume execution again. Do this until stopped.
do {
	// Execute 1000 instructions at a time
	machine.simulate(1000);
	// Do some work
	printf("Instruction count: %zu\n", (size_t) machine.cpu.registers().counter);
} while (!machine.stopped());
```

Note that for the sake of this example we have not wrapped the call to `simulate()` in a try..catch, but if a CPU exception happens, it will throw a `riscv::MachineException`.
