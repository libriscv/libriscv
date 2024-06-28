# Function calls into the guest VM

## Example

It is possible to make arbitrary function calls into the VM guest. The functions must be C ABI, and if you use the string name, the function must be in the symbol table.

Host:
```C++
	struct Four {
		glm::vec3 pos;
		glm::vec3 look;
	};
	Four four;
	int ret = machine.vmcall("test", 111, 222, "333", four);
	printf("test returned %d\n", ret);
```

Guest:
```C++
extern "C" __attribute__((used, retain))
int test(int a, int b, const char* c, Four& four) {
	return a + b;
}
```

This will look up `test` in the symbol table, get the address and use that as the starting address. Further, it will put arguments into registers according to the C ABI, eg. A0-A7 are integral and pointer arguments. A regular function call is performed.

The first argument 111 goes to register A0 and 222 goes to register A1, as they are both integers. "333" will be pushed on the stack and the pointer to the new string on the guests stack is placed in register A2. The struct is pushed by value on stack, and its address is placed in register A3. When the guest program executes the function it will place a + b == 333 in A0, which is the return value register.

We can take Four by reference (`Four&`) or by pointer (`Four*`) inside the guest. Const is also up to you (eg. `const Four *four`), as the stack is writable in the guest, you may just pick what makes the most sense.

## Prerequisites

There are a handful of things that need to be taken care of in order to make function calls into a VM guest program. First off, you should not return normally from `int main()`, as that will call global destructors which will make the run-time environment unreliable for certain programming languages, eg. C++. Instead, one should call `_exit(status)` (or equivalent) from `int main()`, which immediately exits the machine, but does not call global destructors and does not attempt to free resources.

It is not unsafe to call `_exit()` or any other equivalent function directly, as all it does is invoke the `exit` or `exit_group` system calls, which immediately stops execution of the program. In a normal operating system this also makes the execution environment (usually a process) disappear, and releases all the resources back. In this case we just want to preserve the machine state (which is in a good known state) by stopping execution, so that we can call a function.

Once the machine is no longer running, but still left in a state in which we can call into it, we have to make sure that our callable function in the Machine is present in the symbol table of the ELF, so that we can find the address of this function.

A third, and final, stumbling block is sometimes having functions, even those marked with `__attribute__((used))`, not appearing in the ELF symbol table, which is a linker issue. This can happen when using `-gc-sections` and friends. You can test if your symbol is visible to the emulator by using `machine.address_of("myFunction")` which returns a memory address. If the address is 0, then the name was not found in the symbol table, which makes `vmcall(...)` impossible to perform. Calling will likely cause a CPU exception on address 0, which by default has no execute privileges. You can also use `riscv64-***-readelf -a myprogram | grep mysymbol` to check (or even automate the checking).

The simplest solution is to use GCC 11 or later and use a function attribute that retains the symbol, even against pruning: `__attribute__((used, retain))` Be careful about stripping symbols though, because the function attribute only protects it against being pruned by linker GC. You can use `--retain-symbols-file`to only keep the symbols you care about, but it is more advanced to automate.

## Exiting early

Start by running the machine normally and complete `int main()` , but don't return from it, making sure global constructors are called and the C run-time environment is fully initialized, as well as your own things. So, if you are calling `_exit(0)` from main (or stopping the machine another way) you are ready to make function calls into the virtual machine. The primary reason to use `_exit` is because it's a function call, and it also stops the machine. Both of these things are important. The function call makes sure the compiler keeps the stack pristine, and stopping inside the function makes it so that everything we would want later is properly kept. You can even use local stack variables and data to future vmcalls if you stop the machine properly.

## VMCall exact behavior

VM calls should match the C ABI such that when using `machine.vmcall("myfunc", arg1, arg2)`, it is as if you were calling a C function. Example:

```C++
machine.vmcall("my_function", "Hello Sandboxed World!");
```
This function will look up `my_function` in the programs symbol table (expensive), push the string on the stack, and puts its address in A0. After that, it will run the function:

```C++
void my_function(const char* str) {
	printf("%s\n", str);
}
```
Should print `Hello Sandboxed World!\n`.

Take care to distinguish between float and double values properly. There are (in practice) different handling for these. For example:

```C++
machine.vmcall("my_function", 1.0, 2.0, 3.0);
```

Will put 3 double values (64-bit floating point) into registers FA0, FA1 and FA2. The only way to read them is through `double` in C:

```C++
void my_function(double d1, double d2, double d3) {
	printf("%f, %f, %f\n", d1, d2, d3);
}
```

Instead, 1.0f will make them 32-bit floats:
```C++
machine.vmcall("my_function", 1.0f, 2.0f, 3.0f);
```

They are now float arguments, although in C they will get promoted to double for the va_list:
```C++
void my_function(float d1, float d2, float d3) {
	printf("%f, %f, %f\n", d1, d2, d3);
}
```

Currently these argument types are fully supported: Integers, floats, doubles, strings, simple structs.

On 32-bit RISC-V, 64-bit integer arguments will be use two integer registers. Further, a 64-bit return value will use two integer registers (A0, A1).

There is room for 8 integer arguments and 8 floating-point arguments separately. This means you can efficiently call a function with up to 16 arguments, if you need to.

## Return values

Most functions return an integer, so that is the default return value from `machine.vmcall(...)`, however there are other return types. For this we have `machine.return_value<T>`. It's a helper that allows you to return basically anything legal, except 16-byte structs (not yet implemented).

`machine.return_value<T>` supports integers, floats, doubles, C-strings, and pointer to structs.

With the following guest program:
```C
	extern const char* hello() {
		return "Hello World!";
	}

	static struct Data {
		int val1;
		int val2;
		float f1;
	} data = {.val1 = 1, .val2 = 2, .f1 = 3.0f};
	extern struct Data* structs() {
		return &data;
	}
```
We can get the results like so:
```C++
	machine.vmcall("hello");
	printf("%s\n", machine.return_value<std::string>().c_str());

	machine.vmcall("structs");
	const auto* data_ptr = machine.return_value<Data*>();
	assert(data_ptr->val1 == 1);
	assert(data_ptr->val2 == 2);
	assert(data_ptr->f1 == 3.0f);
```


## Function references

Another common method is to register functions directly using a helper system call. After registering all the callback functions, we then call a function that stops the machine directly. At that point you also have access to the original main stack and arguments, but you also don't need to retain functions. You can even use local functions that are not exported. And you don't need to always use the C calling convention, although you should take care to match the calling convention you are selecting instead. It is probably easiest and safest to keep using the C calling convention. Example:

```C
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

```C
inline long register_get_handler(void (*handler) (const char *))
{
	register long a0 asm("a0") = (uintptr_t)handler;
	register long syscall_id asm("a7") = SYSNO_REGISTER_GET;

	asm volatile ("ecall" : "+r"(a0) : "r"(syscall_id) : "memory");

	return a0;
}
inline void wait_for_requests()
{
	register long a0 asm("a0");
	register long syscall_id asm("a7") = SYSNO_WAIT_REQUESTS;

	asm volatile ("ecall" : "=r"(a0) : "r"(syscall_id));
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


## Faster calls with Prepared Calls

Prepared calls can improve latency when binary translation is enabled.

```C++
#include <libriscv/prepared_call.hpp>
...
riscv::Machine<riscv::RISCV64> machine(...);

auto func = machine.address_of("my_function");

// Create a prepared call
riscv::PreparedCall<riscv::RISCV64, void(int, float)> caller(machine, func);
// Make the function call
caller(1, 2.0f);
// Make the function call again
caller(3, 4.0f);
```

## Manual VM call

Here is an example of a manual vmcall that also exits the simulate() call every ~1000 instructions. Maybe you want to do some things in between? This method is used in the [D00M example](/examples/doom/src/main.cpp).

```C++
auto test_addr = machine.address_of("test");

// Reset the stack pointer from any previous call to its initial value
machine.cpu.reset_stack_pointer();
// Reset the instruction counter, as the resume() function will only increment it
machine.reset_instruction_counter();
// Function call setup for the guest VM, but don't start execution
machine.setup_call(555, 666);
machine.cpu.jump(test_addr);
// Run the program for X amount of instructions, then print something, then
// resume execution again. Do this until stopped.
do {
	// Execute 1000 instructions at a time without resetting the counter
	machine.resume<false>(1000);
	// Do some work in between simulation
	printf("Working ...\n");
} while (machine.instruction_limit_reached());
```

The helper function `machine.instruction_limit_reached()` will tell you if the instruction limit was reached during simulation, but it *will not* tell you if the machine stopped normally. Use `machine.stopped()` for that. Combining both helpers you can determine the stopping cause.

## Interrupting a running machine

It is possible to interrupt a running machine to perform another task. This can be done using the `Machine::preempt()` function. A machine can also interrupt itself without any issues. Preemption stores and restores all registers, making it slightly expensive, but guarantees the ability to preempt from any location.
