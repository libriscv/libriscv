# Function calls into the guest VM

## Example

It is possible to make arbitrary function calls into the VM guest. The functions must be C ABI, and if you use the string name, the function must be in the symbol table.

Host:
```C++
	int ret = machine.vmcall("test", 111, 222, "333");
	printf("test returned %d\n", ret);
```

Guest:
```C++
extern "C" __attribute__((used, retain))
int test(int a, int b, const char* c) {
	return a + b;
}
```

This will look up `test` in the symbol table, get the address and use that as the starting address. Further, it will put arguments into registers according to the C ABI, eg. A0-A7 are integral and pointer arguments. A hidden exit function execute segment will be added in the guests memory, and the return address of the call will be to this function that simply exits the VM forwarding the return value(s) from the call. As a result, a regular function call is performed.

The first argument 111 goes to register A0 and 222 goes to register A1, as they are both integers. "333" will be pushed on the stack and the pointer to the new string on the guests stack is placed in A3. When the guest program executes the function it will place a + b == 333 in A0, which is the return value register.

## Prerequisites

There are a handful of things that need to be taken care of in order to make function calls into a VM guest program. First off, you should not return normally from `int main()`, as that will call global destructors which will make the run-time environment unreliable for a few programming languages. Instead, one should call `_exit(status)` (or equivalent) from `int main()`, which immediately exits the machine, but does not call global destructors and frees resources.

It is not unsafe to call `_exit()` or any other equivalent function directly, as all it does is invoke the `exit` or `exit_group` system calls, which immediately stops execution of the program. In a normal operating system this also makes the execution environment (usually a process) disappear, and releases all the resources back. In this case we just want to preserve the machine state (which is in a good known state) by stopping execution, so that we can call a function.

Once the machine is no longer running, but still left in a state in which we can call into it, we have to make sure that our callable function in the Machine is present in the symbol table of the ELF, so that we can find the address of this function.

A third, and final, stumbling block is sometimes having functions, even those marked with `__attribute__((used))`, not appearing in the ELF symbol table, which is a linker issue. This can happen when using `-gc-sections` and friends. You can test if your symbol is visible to the emulator by using `machine.address_of("myFunction")` which returns a memory address. If the address is 0, then the name was not found in the symbol table, which makes `vmcall(...)` impossible to perform. Calling will likely cause a CPU exception on address 0, which by default has no execute privileges. You can also use `riscv64-***-readelf -a | grep mysymbol` to check (or even automate the checking).

The simplest solution is to use GCC 11 or later and use a function attribute that retains the symbol, even against pruning: `__attribute__((used, retain))` Be careful about stripping symbols though, because the function attribute only protects it against being pruned by linker GC. You can use `--retain-symbols-file`to only keep the symbols you care about, but it is more advanced to automate.

## Exiting early

Start by running the machine normally and complete `int main()` , but don't return from it, making sure global constructors are called and the C run-time environment is fully initialized, as well as your own things. So, if you are calling `_exit(0)` from main (or stopping the machine another way) you are ready to make function calls into the virtual machine. The primary reason to use `_exit` is because it's a function call, and it also stops the machine. Both of these things are important. The function call makes sure the compiler keeps the stack pristine, and stopping inside the function makes it so that everything we would want later is properly kept. You can even use local stack variables and data to future vmcalls if you stop the machine properly.

## Function references

Another common method is to register functions directly using a helper system call. After registering all the callback functions, we then call a function that stops the machine directly. At that point you also have access to the original main stack and arguments, but you also don't need to retain functions. You can even use local functions that are not exported. And you don't need to always use the C calling convention, although you should take care to match the calling convention you are selecting instead. It is probably easiest and safest to keep using the C calling convention. Example:

```
static void handle_http_get(const char *url) {
	// ...
}
static void handle_http_post(const char *url, const uint8_t *data, size_t len) {
	// ...
}

int main()
{
	// do stuff ...

	register_get_handler(handle_http_get);
	register_post_handler(handle_http_post);
	wait_for_requests();
}
```

The register functions can be very simple assembly:

```
inline long register_get_handler(void (*handler) (const char *))
{
	register long a0 asm("a0") = (uintptr_t)handler;
	register long syscall_id asm("a7") = SYSNO_REGISTER_GET;

	asm volatile ("scall" : "+r"(a0) : "r"(syscall_id) : "memory");

	return a0;
}
inline void wait_for_requests()
{
	register long a0 asm("a0");
	register long syscall_id asm("a7") = SYSNO_WAIT_REQUESTS;

	asm volatile ("scall" : "=r"(a0) : "r"(syscall_id));
}
```

The idea is to get the address of the `register_get_handler` function to reference it, which will make sure it is not pruned from the executable. Even better, we get the address directly and don't have to look it up in the symbol table. We can then remember it outside of the Machine, so that we can call this function when we are receiving a HTTP GET request.

The `wait_for_requests()` function is intended to simply stop the Machine. It is a system call that calls `machine.stop()`, and it is also recommended to set some boolean `initialized` to true, so that you can verify that the guest program actually called `wait_for_requests()`.

For this to work you will need custom system call handlers, one for each function above, which does the above work. The system calls also need to not collide with any Linux system call numbers if you are using that ABI. This method is the one most likely supported by every single language that compiles down to RISC-V and should be preferred. One improvement one could make in this example is to convert `wait_for_requests()` into a proper function call, so that we are guaranteed that the stack is properly kept and no lifetime issues appear.

Here is a system call wrapper you could use instead using global assembly:
```asm
asm(".global wait_for_requests\n"
"wait_for_requests:\n"
" li a7, " STRINGIFY(SYSNO_WAIT_REQUESTS) "\n"
" ecall\n"
" ret\n");
```
The stringify macro is a common method to turn a `#define` into a string number, baking the system call number into the global assembly. You could, for example, insert a hard-coded number instead.

## Calling a function in the virtual machine guest

Example which calls the function `test` with the arguments `555` and `666`:
```C++
	int ret = machine.vmcall("test", 555, 666);
	printf("test returned %d\n", ret);
```

Arguments are passed as normal. The function will move integral values and pointers into the integer registers, and floating-point values into the FP registers. Structs and strings will be pushed on stack and then an integer register will hold the pointer, following the RISC-V C calling convention.

```C++
	struct PodTest {
		int32_t a = 4;
		int64_t b = 6;
	} test;
	int ret = machine.vmcall("test", "This is a string", test);
	printf("test returned %d\n", ret);
```

You can specify a maximum of 8 integer and 8 floating-point arguments, for a total of 16. Instruction counters and registers are not reset on calling functions, so make sure to take that into consideration if you are counting instructions.

## Manual VM call

Here is an example of a manual vmcall that also exits the simulate() call every ~1000 instructions. Maybe you want to do some things in between? This method is used in the [D00M example](/examples/doom/src/main.cpp).

```C++
auto test_addr = machine.address_of("test");

// Reset the stack pointer from any previous call to its initial value
machine.cpu.reset_stack_pointer();
// Function call setup for the guest VM, but don't start execution
machine.setup_call(test_addr, 555, 666);
// Run the program for X amount of instructions, then print something, then
// resume execution again. Do this until stopped.
do {
	// Execute 1000 instructions at a time
	machine.simulate<false>(1000);
	// Do some work in between simulation
	printf("Working ...\n");
} while (machine.instruction_limit_reached());
```

The helper function `machine.instruction_limit_reached()` will tell you if the instruction limit was reached during simulation, but it *will not* tell you if the machine stopped normally. Use `machine.stopped()` for that. Combining both helpers you can determine the stopping cause.

## Interrupting a running machine

It is possible to interrupt a running machine to perform another task. This can be done using the `Machine::preempt()` function. A machine can also interrupt itself without any issues. Preemption stores and restores all registers, making it slightly expensive, but guarantees the ability to preempt from any location.
