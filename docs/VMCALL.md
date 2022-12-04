# Function calls into the guest VM

## Setup

It's possible to make function calls into the virtual machine, but there are a handful of things that need to be taken care of in order to do that. First off, you should not return from `int main()`, as that will call global destructors which will make the run-time environment unreliable for several programming languages. Instead, one should call `_exit(status)` (or equivalent) from `int main()`, which immediately exits the machine, but does not call global destructors and free resources.

It is not unsafe to call `_exit()` or any other equivalent function directly, as all it does is invoke the EXIT system call, which immediately stops execution of the program. In a normal operating system this also makes the execution environment (usually a process) disappear, and releases all the resources back. In this case we just want to preserve the machine state (which is in a good known state) while also stopping execution, so that we can call into the programs functions directly.

Once the machine is no longer running, but still left in a state in which we can call into it, we have to make sure that our callable function in the Machine is present in the symbol table of the ELF, so that we can find the address of this function.

A third, and final, stumbling block is sometimes having functions, even those marked with `__attribute__((used))`, not appearing in the ELF symbol table, which is a linker issue. This can happen when using `-gc-sections` and friends. You can test if your symbol is visible to the emulator by using `machine.address_of("myFunction")` which returns a memory address. If the address is 0, then the name was not found in the symbol table, which makes `vmcall(...)` impossible to perform. Calling will most likely cause a CPU exception on address 0, which by default has no execute privileges. You can also use `riscv64-***-readelf -a | grep mysymbol` to check (or even automate the checking).

The simplest solution is to use GCC 11 or later and use a function attribute that retains the symbol, even against pruning: `__attribute__((used, retain))` Be careful about stripping symbols though, because the function attribute only protects it against being pruned by linker GC. You can use `--retain-symbols-file`to only keep the symbols you care about, but it is more advanced to automate.

## Exiting early

Start by running the machine normally and complete `int main()` , but don't return from it, making sure global constructors are called and the C run-time environment is fully initialized, as well as your own things. So, if you are calling `_exit(0)` from main (or stopping the machine another way) you are ready to make function calls into the virtual machine. The primary reason to use `_exit` is because it's a function call, and it also stops the machine. Both of these things are important. The function call makes sure the compiler keeps the stack pristine, and stopping inside the function makes it so that everything we would want later is properly kept. You can even pass local stack variables and data to future vmcalls if you stop the machine properly.

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

## Calling a function in the guest virtual machine

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

You can specify a maximum of 8 integer and 8 floating-point arguments, for a total of 16. Instruction counters and registers are not reset on calling functions, so make sure to take that into consideration when measuring.

It is not recommended to copy data into guest memory and then pass pointers to this data as arguments, as it's a very complex task to determine which memory is unused by the guest before and even during the call. Instead, the guest can allocate room for the struct on its own, and then simply perform a system call where it passes a pointer to the struct, as well as the length as arguments. That is a very common way to handle data between user- and host- address spaces.

If there is a lot of data, you could also share the data as pages with the guest, and then pass the virtual address as argument instead. The data is only required to be 8-byte aligned similar to what the heap returns anyway, but you must make sure size % PageSize == 0. That means, you will need to share data that is page-sized, one or more whole pages.

## Doing work in-between machine execution

Without using threads the machine program will simply run until it's completed, an exception occurs, or the machine is stopped from the outside during a trap or system call. It would be nice to have the ability to run the host-side program in-between without preemption. We can do this by making vmcall not execute machine instructions, and instead do it ourselves manually:

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

## Maximizing success and optimizing calls

Check that the symbol exists using the `Machine::address_of()` function. If you want to cache the address, there is a helper class called [CachedAddress](../lib/libriscv/cached_address.hpp).

Build your executable with `-O2 -march=rv32g -mabi=ilp32d` or `-O2 -march=rv32imfd -mabi=ilp32` based on measurements. Soft-float is always slower. Accelerate the heap by managing the chunks from the outside using system calls. There is no need for any mmap functionality. Use custom linear arenas for the page hashmap if you require instant machine deletion.

Use `Memory::memview()` or `Memory::memstring()` to handle arguments passed from guest to host. They have fast-paths for strings and structs that don't cross page-boundaries. Use `std::deque` instead of `std::vector` inside the guest where possible. The fastest way to append data to a list is using a `std::array` with a pointer: `*ptr++ = value;`.

## Interrupting a running machine

It is possible to interrupt a running machine to perform another task. This can be done using the `Machine::preempt()` function. A machine can also interrupt itself without any issues. Preemption stores and restores all registers, making it slightly expensive, but guarantees the ability to preempt from any location.
