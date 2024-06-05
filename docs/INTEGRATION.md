# Integration

Integrating libriscv into your project is fairly straight-forward, but employing it as a low-latency scripting solution is of higher difficulty. There is an example of how to do this in my [RVScript repository](https://github.com/fwsGonzo/rvscript). There is also a simpler version of this in this repostiryo, [the gamedev examples](/examples/gamedev).

Despite the complexity required, let's just go through everything from the start.

## Embedding libriscv

You add libriscv primarily through CMake. Add it like so:

```sh
mkdir -p ext
cd ext
git submodule add git@github.com:fwsGonzo/libriscv.git
```

Now create a CMakeLists.txt in the ext folder:

```cmake
# libriscv in 64-bit mode with read-write arena
option(RISCV_32I "" OFF)
option(RISCV_64I "" ON)
option(RISCV_FLAT_RW_ARENA "" ON)

add_subdirectory(libriscv/lib)
# We need to make room for our own system calls, as well as
# the classic Linux system calls (0-500). So ours are from 500-600.
target_compile_definitions(riscv PUBLIC RISCV_SYSCALLS_MAX=600)
```

We can now use this ext/CMakeLists.txt from our root CMakeLists.txt:

```cmake
add_subdirectory(ext)

...
target_link_libraries(myprogram riscv)
```

libriscv is now accessible in the code:

```C++
#include <libriscv/machine.hpp>

int main()
{
    riscv::Machine<riscv::RISCV64> machine(binary_vec_u8);
}
```

You can see how RVScript does the same thing [here](https://github.com/fwsGonzo/rvscript/blob/master/ext/CMakeLists.txt).

The engine subfolder is adding libriscv [here](https://github.com/fwsGonzo/rvscript/blob/master/engine/CMakeLists.txt#L46).

Note that you can also install libriscv through packaging, eg. `libriscv` on AUR.

## Compiling a RISC-V program

There are compilers for RISC-V that comes with Linux distributions, however they are not the most performant way to go. They primarily use RV64GC, which is not the fastest way to emulate RISC-V, but still quite acceptable. If you have the choice, clone the riscv-gnu-toolchain and compile for Newlib:

```sh
./configure --prefix=$HOME/riscv --with-arch=rv64g_zba_zbb_zbc_zbs --with-abi=lp64d
make
```

Or, for 32-bit:
```sh
./configure --prefix=$HOME/riscv --with-arch=rv32g_zba_zbb_zbc_zbs --with-abi=ilp32d
make
```

Now adding `$HOME/riscv/bin` to your PATH will expose a custom built RISC-V compiler.
Add this to `.bashrc`:

```sh
export PATH=$PATH:$HOME/riscv/bin
```

You should now be able to execute this from command-line:

```sh
$ riscv64-unknown-elf-g++ --version
riscv64-unknown-elf-g++ (gc891d8dc23e) 13.2.0

$ riscv32-unknown-elf-g++ --version
riscv32-unknown-elf-g++ (gc891d8dc23e) 13.2.0
```

Note that compiling your own RISC-V compiler is completely optional. _libriscv_ is fully compatible with any local RISC-V compilers in your packaging system, and compatible with most if not all systems languages (C/C++, Zig, Rust, ...).

## Compiling a basic program

Using this simple C program:
```C
#include <stdio.h>
#define STR(x) #x

__attribute__((used, retain))
int my_function(int arg)
{
	printf("Hello " STR(__FUNC__) " World! Arg=%d\n", arg);
	return arg;
}
int main()
{
	printf("Hello World!\n");
}
```

We can compile it like so:
```sh
riscv64-unknown-elf-gcc -static -O2 myprogram.cpp -o myprogram
```

We always compile statically, in order for everything (all dependencies) to be available to us inside the program. The program will be self-contained.

Now we can run through `main()` and we can also make a function call to `my_function`:

```C++
#include <libriscv/machine.hpp>

int main()
{
	// Create 64-bit RISC-V machine using loaded program
    riscv::Machine<riscv::RISCV64> machine(binary_vec_u8);

	// Add POSIX system call interfaces (no filesystem or network access)
	machine().setup_linux_syscalls(false, false);
	machine().setup_posix_threads();

	// setup program argv *after* setting new stack pointer
	machine().setup_linux({"my_program", "arg0"}, {"LC_ALL=C"});

	// Run through main()
	try {
		machine().simulate();
	} catch (const std::exception& e) {
		fprintf(stderr, "Exception: %s\n", e.what());
	}

	// Call a function (as long as it's in the symbol table)
	int ret = machine().vmcall("my_function", 123);

	// Forward return value from function
	return ret;
}
```

Note: If you strip the program, you cannot call even retained functions. Use a linker option to strip all symbols except the ones you care about instead from a text file: `-Wl,--retain-symbols-file=symbols.txt`. Alternatively, only strip debug symbols. Debug information is often the largest contributor to file size.

# Advanced features

These features are already implemented in [RVScript](https://github.com/fwsGonzo/rvscript), but I am briefly detailing how it works and how to implement it here.

## Dynamic calls (guest-side)

In order for the script to be useful we can't only focus on making function calls into the sandboxed program. We also want to make calls from the program and back into the host (eg. game engine) in order to ask for stuff, or ask the game engine to do something. For example to create a timer.

Dynamic calls are an integral part of a low-friction scripting framework, but they require a bit of work to integrate. The best way to understand how they are generated and then used in the script, is to read the code:

The [python script](https://github.com/fwsGonzo/rvscript/blob/master/programs/dyncalls/generate.py) that reads [dynamic_calls.json](https://github.com/fwsGonzo/rvscript/blob/master/programs/dynamic_calls.json) and outputs callable functions and inline assembly variants.

In order to re-generate the API every time dynamic_calls.json is changed, we use a simple call to [add_dependencies()](https://github.com/fwsGonzo/rvscript/blob/master/programs/micro/micro.cmake#L58)

In order to rebuild the program each time the API changes, we add the [generated sources to the build list](https://github.com/fwsGonzo/rvscript/blob/master/programs/micro/micro.cmake#L51-L56). Notice how they are explicitly marked as GENERATED.

Once all the sources are generated, the dynamic call API can be [included in the guest programs](https://github.com/fwsGonzo/rvscript/blob/master/programs/micro/api/api.h#L7).

And finally, we can [use all the dynamic calls](https://github.com/fwsGonzo/rvscript/blob/master/programs/micro/api/api_impl.h#L149) we specified in the JSON file. By using, I mean implementing a helper wrapper function in the program running inside the sandbox.

The dynamic call python script will generate the exact function written down. For example, for creating a timer the dynamic call signature is `"int sys_timer_periodic (float, float, timer_callback, void*, size_t)"`. That means, inside the sandbox you can now use `sys_timer_periodic`, however it's not a nice API on its own. Let's write a helper function for it:

```C++
#include <dyncall_api.h> // timer_callback, sys_timer_periodic

struct Timer {
	using TimerCallback = void (*)(Timer);

	/// @brief Create a timer that calls a function after the given seconds,
	/// then periodically gets called again after the given period (also in seconds).
	static Timer periodic(float seconds, float period, TimerCallback callback)
	{
		return {sys_timer_periodic(seconds, period, [] (int id, void* cb) {
			// Cast cb pointer to our callback type, and construct a timer from the ID as arg
			((TimerCallback)cb)(Timer(id));
		}, callback, sizeof(callback))};
	}

	int id;
};
```
Using this tiny wrapper and without any fancy std::function-like types we have created a wrapper for timer creation. We can now use it like so:

```C++
auto t = Timer::periodic(5.0f, [] (Timer t) {
	print("Hello from timer ", t.id, "!\n");
});

```
Also, using the periodic wrapper function we can create many more helper functions, like oneshot timers:

```C++
struct Timer {
	using TimerCallback = void (*)(Timer);

	static Timer periodic(float seconds, float period, TimerCallback callback) { ... }

	static Timer periodic(float period, TimerCallback callback) {
		return periodic(0.0f, seconds, callback);
	}

	static Timer oneshot(float seconds, TimerCallback callback) {
		return periodic(seconds, 0.0f, callback);
	}

	int id;
};
```
Now we have a decent Timer API inside the sandbox.


## Implementing a dynamic call handler (host-side)

This part is intentionally very low-friction. Adding dynamic calls means assigning a callback to a (string) function definition:

```C++
	Script::set_dynamic_calls({
		{"Timer::stop", "void sys_timer_stop (int)",
		 [](Script& script)
		 {
			 // Stop timer
			 const auto [timer_id] = script.machine().sysargs<int>();
			 timers.stop(timer_id);
		 }},
		{"Timer::periodic", "int sys_timer_periodic (float, float, timer_callback, void*, size_t)",
		 [](Script& script)
		 {
			 // Periodic timer
			 auto& machine = script.machine();
			 const auto [time, peri, addr, data, size]
				 = machine.sysargs<float, float, gaddr_t, gaddr_t, gaddr_t>();

			 auto capture = CaptureStorage::get(machine, data, size);

			 int id = timers.periodic(
				 time, peri,
				 [addr = (gaddr_t)addr, capture, script = &script](int id)
				 {
					 script->call(addr, id, capture);
				 });
			 machine.set_result(id);
		 }},
	});
```
The friendler `Timer::stop` and `Timer::periodic` is only used when an exception happens in order to make errors more readable.


# Safety and predictability

Dynamic call implementations in the host and the table in the guest program identify each others only using the function definition strings (and only that): `"void sys_timer_stop (int)"` and `"int sys_timer_periodic (float, float, timer_callback, void*, size_t)"`, in this case.

If any of the definitions change, they will no longer find each other, and you will be notified if anyone tries to call an unhandled dynamic call. So if there is a mismatch in the definitions between the program and the host engine, they won't be able to see each others, but you will be able to see what they are trying to do when it fails.

It is designed this way to catch:

- Mismatching arguments, even mismatching argument names
- Being able to write out which functions are missing/unimplemented
- If an exception is called when handling a dynamic call, we can print the name and the definition
- Avoid collisions

It's definitely a very verbose API, however that pays off when integrating this and when debugging later on.
