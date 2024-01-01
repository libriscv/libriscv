# Integration

Integrating libriscv into your project is fairly straight-forward, but employing it as a low-latency scripting solution is of higher difficulty. There is an example of how to do this in my [RVScript repository](https://github.com/fwsGonzo/rvscript).

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

## Compiling a RISC-V program

There are compilers for RISC-V that comes with Linux distributions, however they are not the most performant way to go. They primarily use RV64GC, which is not the fastest way to emulate RISC-V. If you have the choice, clone the riscv-gnu-toolchain and compile for Newlib:

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

## Compiling a program

Using this program:
```C++
#include <cstdio>
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
riscv64-unknown-elf-g++ -static -O2 myprogram.cpp -o myprogram
```

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
	machine().simulate();

	// Call a function (as long as it's in the symbol table)
	int ret = machine().vmcall("my_function", 123);

	// Forward return value from function
	return ret;
}
```

Note: If you strip the program, you cannot call even retained functions. Use a linker option to strip all symbols except the ones you care about instead from a text file: `-Wl,--retain-symbols-file=symbols.txt`.

# Advanced features

These features are already implemented in [RVScript](https://github.com/fwsGonzo/rvscript), but I am briefly detailing how it works and how to implement it here.

## Dynamic calls (guest-side)

Dynamic calls are an integral part of a low-friction scripting framework, but they require a bit of work to integrate. The best way to understand how they are generated and then used in the script, is to read the code:

The [python script](https://github.com/fwsGonzo/rvscript/blob/master/programs/dyncalls/generate.py) that reads [dynamic_calls.json](https://github.com/fwsGonzo/rvscript/blob/master/programs/dynamic_calls.json) and outputs callable functions and inline assembly variants.

In order to re-generate the API every time dynamic_calls.json is changed, we use a simple call to [add_dependencies()](https://github.com/fwsGonzo/rvscript/blob/master/programs/micro/micro.cmake#L58)

In order to rebuild the program each time the API changes, we add the [generated sources to the build list](https://github.com/fwsGonzo/rvscript/blob/master/programs/micro/micro.cmake#L51-L56). Notice how they are explicitly marked as GENERATED.

Once all the sources are generated, the dynamic call API can be [included in the guest programs](https://github.com/fwsGonzo/rvscript/blob/master/programs/micro/api/api.h#L7).

And finally, we can [use all the dynamic calls](https://github.com/fwsGonzo/rvscript/blob/master/programs/micro/api/api_impl.h#L149) we specified in the JSON file:

```C++
#include <dyncall_api.h>
...
using timer_callback = void (*)(int, void*);

inline Timer timer_periodic(
	float time, float period, timer_callback callback, void* data, size_t size)
{
	return {sys_timer_periodic(time, period, callback, data, size)};
}
```

`sys_timer_periodic` is listed as:
```json
"Timer::periodic": "int sys_timer_periodic (float, float, timer_callback, void*, size_t)"
```

This concludes the integration in the guest program. The guest programs should now have a dynamic call table, as well as opaque functions for each dynamic call.

## Dynamic calls (host-side)

Assuming you have already created a wrapper around the RISC-V machines, and loaded a program, run through it's main() function etc.:

```C++
void Script::resolve_dynamic_calls()
{
	this->m_g_dyncall_table = machine().address_of("dyncall_table");
	if (m_g_dyncall_table == 0x0)
		throw std::runtime_error(this->name() + ": Unable to find dynamic call table");
	// Table header contains the number of entries
	const uint32_t entries = machine().memory.read<uint32_t> (m_g_dyncall_table);
	if (entries > 512)
		throw std::runtime_error(this->name() + ": Too many dynamic call table entries (bogus value)");
	// Skip past header
	const auto g_table = m_g_dyncall_table + 0x4;

	// Reserve space for host-side dynamic call handlers
	this->m_dyncall_array.reserve(entries);
	this->m_dyncall_array.clear();
	if constexpr (WARN_ON_UNIMPLEMENTED_DYNCALL) {
	strf::to(stderr)(
		"Resolving dynamic calls for '", name(), "' with ", entries, " entries\n");
	}

	// Copy whole table into vector
	std::vector<DyncallDesc> table (entries);
	machine().copy_from_guest(table.data(), g_table, entries * sizeof(DyncallDesc));

	for (unsigned i = 0; i < entries; i++) {
		auto& entry = table.at(i);

		auto it = m_dynamic_functions.find(entry.hash);
		if (LIKELY(it != m_dynamic_functions.end()))
		{
			this->m_dyncall_array.push_back(it->second.func);
		} else {
			this->m_dyncall_array.push_back(
			[] (auto&) {
				throw std::runtime_error("Unimplemented-trap");
			});
			if constexpr (WARN_ON_UNIMPLEMENTED_DYNCALL) {
			const std::string name = machine().memory.memstring(entry.strname);
			strf::to(stderr)(
				"WARNING: Unimplemented dynamic function '", name, "' with hash ", strf::hex(entry.hash), " and program table index ",
				m_dyncall_array.size(), "\n");
			}
		}
	}
	if (m_dyncall_array.size() != entries)
		throw std::runtime_error("Mismatching number of dynamic call array entries");
}
```
The goal is to read the table in the guest program, and for each unimplemented dynamic call, throw a specific exception, so that we can resolve it at run-time.

In order to be able to handle a dynamic call we must register a custom instruction.

```C++
	// A custom intruction used to handle indexed dynamic calls.
	using namespace riscv;
	static const Instruction<MARCH> dyncall_instruction_handler {
		[](CPU<MARCH>& cpu, rv32i_instruction instr)
		{
			auto& scr = script(cpu.machine()); // Cast the user pointer in machine to Script&
			scr.dynamic_call_array(instr.Itype.imm); // Trigger dynamic call using index
		},
		[](char* buffer, size_t len, auto&, rv32i_instruction instr)
		{
			return snprintf(
				buffer, len, "DYNCALL: 4-byte 0x%X (0x%X)", instr.opcode(),
				instr.whole);
		}};

	// Override the machines unimplemented instruction handling,
	// in order to use the custom instruction instead.
	CPU<MARCH>::on_unimplemented_instruction
		= [](rv32i_instruction instr) -> const Instruction<MARCH>&
	{
		if (instr.opcode() == 0b1011011)
		{
			return dyncall_instruction_handler;
		}
		return CPU<MARCH>::get_unimplemented_instruction();
	};
```
With this custom unimplemented (unknown) instruction handler, we can recognize the dynamic call instruction 0b1011011.

Finally, we can handle the dynamic call by its index:
```C++
void Script::dynamic_call_array(uint32_t idx)
{
	while (true) {
		try {
			this->m_dyncall_array.at(idx)(*this);
			return;
		} catch (const std::exception& e) {
			// This will re-throw unless a new dynamic call is discovered
			this->dynamic_call_error(idx, e);
		}
	}
}
```
Then, dynamic_call_error (which should be a cold function), can print which dynamic call failed by checking which call is at which index. The purpose of the while() loop is to allow resolving a dynamic call that didn't exist at setup time:

```C++
void Script::dynamic_call_error(uint32_t idx, const std::exception& e)
{
	const uint32_t entries = machine().memory.read<uint32_t> (m_g_dyncall_table);
	if (idx < entries) {
		DyncallDesc entry;
		const auto offset = 0x4 + idx * sizeof(DyncallDesc);
		machine().copy_from_guest(&entry, m_g_dyncall_table + offset, sizeof(DyncallDesc));

		// Try to resolve it again
		if (e.what() == std::string("Unimplemented-trap"))
		{
			auto it = m_dynamic_functions.find(entry.hash);
			if (LIKELY(it != m_dynamic_functions.end()))
			{
				// Resolved, return directly
				this->m_dyncall_array.at(idx) = it->second.func;
				return;
			}
		}

		const auto dname = machine().memory.memstring(entry.strname);
		strf::to(stderr)(
			"ERROR: Exception in '", this->name(),"', dynamic function '", dname, "' with hash ",
			strf::hex(entry.hash), " and table index ", idx, "\n");

	} else {
		strf::to(stderr)(
			"ERROR: Exception in '", this->name(),"', dynamic function table index ",
			idx, " out of range\n");
	}
	throw;
}
```

Using this method we can handle dynamic calls both that existed at setup-time and also lazily install them as needed. Obviously, it is not ideal to use exceptions for this, but this way resulted in the lowest latency for me.

Briefly, the order again:

1. Read the dynamic call table of the program
2. Assign dynamic call functions for each implemented and unimplemented call
3. Optionally lazily resolve unimplemented dynamic calls at run-time
4. Add custom instruction handler by overriding the global default unknown-instruction callback
5. Invoke a dynamic call using the immediate value of the instruction

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
				 [addr = (gaddr_t)addr, capture, &script](int id)
				 {
					 gaddr_t dst = script.guest_alloc(capture.size());
					 script.machine().copy_to_guest(
						 dst, capture.data(), capture.size());
					 script.call(addr, id, dst);
					 script.guest_free(dst);
				 });
			 machine.set_result(id);
		 }},
	});
```
The friendler `Timer::stop` and `Timer::periodic` is only used when an exception happens in order to make errors more readable.

Dynamic call implementations in the host and the table in the guest program identify each others only using the function definition strings: `"void sys_timer_stop (int)"` and `"int sys_timer_periodic (float, float, timer_callback, void*, size_t)"`.

If any of the definitions change, they will no longer find each other.


## Adding custom virtual memory

```C++
using addr_t = riscv::address_type<riscv::RISCV64>;

// Memory area shared between all script instances
static constexpr addr_t SHM_BASE	= 0x2000;
static constexpr addr_t SHM_SIZE	= 2 * riscv::Page::size();
static std::array<uint8_t, SHM_SIZE> shared_memory {};

void insert_shared_memory()
{
	// Shared memory area between all programs
	machine.memory.insert_non_owned_memory(SHM_BASE, &shared_memory[0], SHM_SIZE);
}
```

After inserting these shared pages, they will be accessible in each machine from 0x2000 to 0x4000.

