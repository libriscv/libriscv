# libriscv Architecture Guide

This document explains libriscv from fundamentals to production integration. libriscv is a RISC-V userspace emulator designed for embedding scripting in C++ applications. Host-guest communication has zero overhead under correct configuration because the host reads and writes guest memory directly through pointers, with no serialization or copying. Further, VM call overhead can be as low as ~5ns.

## Table of Contents

1. [Machine Fundamentals](#machine-fundamentals)
2. [Memory Architecture](#memory-architecture)
3. [Configuring the Emulator](#configuring-the-emulator)
4. [Booting and Initialization](#booting-and-initialization)
5. [Host Functions (Syscalls)](#host-functions-syscalls)
6. [Calling Guest Functions](#calling-guest-functions)
7. [Heap Takeover](#heap-takeover)
8. [Guest Datatypes](#guest-datatypes)
9. [Generated Host Functions](#generated-host-functions)
10. [Advanced: Zero-Copy Structures](#advanced-zero-copy-structures)
11. [Production Integration Example](#production-integration-example)
12. [Minimal Script Wrapper (End-to-End)](#minimal-script-wrapper-end-to-end)
13. [Verifying VMCall Latency](#verifying-vmcall-latency)
14. [RPC Between Same-Program Instances](#rpc-between-same-program-instances)

---

## Machine Fundamentals

A `Machine<W>` holds a static RISC-V ELF program. `W` is the pointer width: `4` for RV32, `8` for RV64. The machine contains a CPU with 32 integer registers + 32 float registers + PC, a memory subsystem, an optional native heap arena, and a syscall handler table.

```cpp
#include <libriscv/machine.hpp>
using machine_t = riscv::Machine<8>; // 64-bit RISC-V
using gaddr_t = riscv::address_type<8>;

// Load ELF binary
std::vector<uint8_t> binary = load_file("program.elf");
machine_t machine(binary, {
    .memory_max = 64ULL << 20,  // 64 MB
    .stack_size = 1ULL << 20,   // 1 MB
});
```

### Constructor Variants

| Constructor | Use Case |
|---|---|
| `Machine(binary, opts)` | Normal: load ELF from binary data |
| `Machine(other_machine, opts)` | Forking: CoW clone for isolation or threading |
| `Machine(opts)` | Empty machine (no program, manual setup) |

The forking constructor creates a thin copy-on-write clone. The parent machine must outlive all forks. Forks share read-only pages and the decoded execute segment, consuming minimal memory. A full fork-execute-destruct cycle completes in under a microsecond, but *requires* virtual paging and *cannot work* with a flat arena.

### Execution Model

There is no on/off state. It is just memory and registers. Execution happens when you call one of the simulation functions, which run instructions until a stopping condition is met:

| Function | Stops when | Use case |
|---|---|---|
| `machine.simulate(max)` | Instruction counter reaches `max`, or STOP is hit | Booting `main()` |
| `machine.vmcall<MAX>(addr, args...)` | Instruction counter reaches `MAX`, or guest function returns | Calling guest functions with a timeout |
| `machine.vmcall<0>(addr, args...)` | Guest function returns (no counter check) | Calling guest functions without a timeout |
| `cpu.simulate_inaccurate(pc)` | STOP is hit (no counter check) | Lowest-overhead execution |

When the `vmcall` template parameter is `0` or `UINT64_MAX`, it internally uses `CPU::simulate_inaccurate()`, a fast-path that skips instruction counting entirely. No counter increment, no limit comparison on every instruction. This is often 15% faster than with instruction counting.

The instruction counter exists to catch runaway execution — infinite loops, not security exploits. A guest stuck in a loop cannot steal data or escape the sandbox; it can only hog CPU time. Instruction counting is most useful during development when guest code might get stuck, or in environments where it is important that no single call runs too long. Syscall handlers can add penalties to the counter (e.g., `malloc` adds thousands of instructions) to account for host-side work that the counter wouldn't otherwise see.

Between calls, the machine retains all state: registers, memory, stack, heap. You call `vmcall`, it runs, the guest function returns (hitting the exit function), and the machine is immediately ready for the next call. There is no teardown or re-initialization.

---

## Memory Architecture

libriscv has two memory modes. The choice affects every subsequent API decision.

### Flat Read-Write Arena (default, recommended)

All guest memory lives in one contiguous host allocation. Memory access is a bounds check + pointer dereference. This enables:

- **Direct pointer access**: `machine.memory.memarray<T>(addr, count)` returns a `T*` into guest memory. No copy. You read and write through it.
- **String views**: `machine.memory.memview(addr, len)` returns a `std::string_view` of guest memory. No copy.
- **Span access**: `machine.memory.memspan<T>(addr, count)` returns a `std::span<T>`. No copy.
- **Alignment checks**: All typed access checks alignment. Misaligned access throws.
- **Protection**: Write attempts to read-only segments throw.

The flat arena is why libriscv has zero-overhead host-guest communication. You are looking at the same bytes the guest sees.

### Const vs Non-Const Memory Access

The template parameter `T` in `memarray<T>`, `memspan<T>`, and related functions controls read-only vs read-write access. This is enforced at the memory level, not just the type system:

- **`memarray<const T>(addr, count)`** uses `memview()` internally, which allows access to the full readable range including `.rodata` and `.text` segments.
- **`memarray<T>(addr, count)`** (non-const T) uses `writable_memview()` internally, which **rejects** read-only segments. Attempting to view `.rodata` through a non-const type triggers a protection fault.

The same rule applies to `memspan<const T>` vs `memspan<T>`.

This is an anti-footgun: you cannot accidentally get a writable pointer to read-only data. If a host function needs to read a guest struct from rodata without modifying it, use `const` in the template parameter. If it needs to modify the struct, use non-const — and the memory system will verify the address is in a writable region.

```cpp
// Reading from rodata — works
auto* data = machine.memory.memarray<const MyStruct>(addr, 1);

// Writing to rodata — protection fault (intentional)
auto* data = machine.memory.memarray<MyStruct>(rodata_addr, 1);  // THROWS

// Reading from heap/stack — both work
auto* ro = machine.memory.memarray<const MyStruct>(heap_addr, 1);  // OK
auto* rw = machine.memory.memarray<MyStruct>(heap_addr, 1);       // OK
```

### Virtual Paging

Each 4KB page is allocated independently. Memory is not guaranteed to be contiguous across page boundaries. You **cannot** get a pointer spanning two pages. Instead:

- `machine.memory.memcpy_out(host_dst, guest_addr, len)` copies guest memory to a host buffer.
- `machine.memory.memcpy(guest_dst, host_src, len)` copies host data into guest memory.
- `machine.memory.read<T>(addr)` / `machine.memory.write<T>(addr, val)` for single values.

Virtual paging is used for constrained environments: hundreds of thousands of concurrent instances where per-instance memory must be minimal. Pages can be stack-allocated or drawn from a workspace arena and dropped after a request.

### Decision Matrix

| Scenario | Memory Mode | Why |
|---|---|---|
| Game scripting, server logic | Flat arena | Direct memory access, zero-copy types |
| Mass-scale microservices | Virtual paging + forking | Minimal per-instance memory via CoW |
| Development/debugging | Flat arena | Simpler, faster iteration |
| Per-request isolation | Virtual paging + forking | Instance on-demand from master VM |

---

## Configuring the Emulator

`MachineOptions<W>` controls everything. Key fields:

```cpp
riscv::MachineOptions<8> opts {
    .memory_max = 28ULL << 20,    // Max guest memory (28 MB)
    .stack_size = 1ULL << 20,     // Stack (1 MB)
    .enforce_exec_only = true,    // X segments cannot be read (security)
    .default_exit_function = "fast_exit",  // Function that stops simulation

    // JIT / Binary Translation (development only)
    .translate_enabled = false,   // NOT safe for sandboxing
    .translate_invoke_compiler = false,
};
```

**JIT (binary translation)**: Translates RISC-V to native code for speed. Enable during development for faster iteration. Disable in production if sandboxing is a security requirement, as JIT-generated code executes natively. JIT produces `.so` or `.dll` files that can be cached across runs.

---

## Booting and Initialization

After construction, execute the guest's `main()`:

```cpp
machine.simulate(MAX_BOOT_INSTRUCTIONS); // e.g., 256'000'000
```

**Critical: Do not let `main()` return.** If `main()` returns, the C runtime runs global destructors, closes stdout/stdin, and tears down the heap. The machine becomes unusable for subsequent calls.

The correct pattern is to have the guest call a host function that pauses execution:

```cpp
// Guest code (RISC-V side)
int main() {
    // Initialize everything...
    wait_for_requests(my_callback);  // <-- pauses here, never returns
}
```

```cpp
// Host side: install the "wait" syscall
machine_t::install_syscall_handler(ECALL_WAIT_FOR_REQUESTS,
[](machine_t& machine) {
    auto [type, address] = machine.sysargs<int, gaddr_t>();
    // Store the callback address for later vmcalls
    script.set_callback_address(address);
    // Stop simulation - guest is now initialized
    machine.stop();
});
```

This solves two problems:
1. The guest referenced the callback function, so the linker won't strip it.
2. The guest is paused mid-execution with all state intact, ready for vmcalls.

After `simulate()` returns, check `machine.instruction_limit_reached()` to detect runaway programs.

If you want to use the pause-point in a while loop (very common with JavaScript), avoid using `vmcall()` and related functions altogether, as they will clobber state needed by the event loop. However, if you still want to make function calls, set the stack initial address to the current stack address: `machine.memory.set_stack_initial(machine.cpu.reg(riscv::REG_SP))`. And then always use `machine.preempt()` instead of `machine.vmcall()`, as preempt saves registers before and restores them after the call, even if an exception happens. This comes with the assumption that the stack doesn't grow in the guests event loop. If that's possible, use mmap-allocate to create a new vmcall-stack instead: `machine.memory.set_stack_initial(machine.memory.mmap_allocate(65536u))` will create a 64kb stack for calls.

---

## Host Functions (Syscalls)

Host functions let the guest call into the host. There are two mechanisms: **syscalls** (ECALL instruction, dispatched by number) and **generated host functions** (custom instruction, dispatched by index). Syscalls are used for system-level services. Generated host functions are the production approach for application-level APIs — see [Generated Host Functions](#generated-host-functions).

### Syscall Registration

```cpp
machine_t::install_syscall_handler(500, [](machine_t& machine) {
    auto [name, value] = machine.sysargs<std::string_view, int>();
    machine.set_result(0);
});
```

### Reading Guest Arguments

`sysargs<T...>()` reads arguments from registers following the RISC-V calling convention (A0-A7 for integers, FA0-FA7 for floats):

```cpp
auto [x] = machine.sysargs<int>();
auto [x, y] = machine.sysargs<float, float>();
auto [name] = machine.sysargs<std::string_view>();  // Zero-copy (flat arena only)
auto [name] = machine.sysargs<std::string>();        // Copies string
auto [ptr] = machine.sysargs<MyStruct*>();           // Direct pointer (flat arena only)
```

Note that if a custom instruction is used to execute host functions, A7 is not needed nor used for the system call number and is a free argument register, making it possible to invoke with 8 integral arguments.

### Setting Return Values

```cpp
machine.set_result(42);              // Single return in A0
machine.set_result(3.14f);          // Float return in FA0
```

### Built-in Syscall Sets

```cpp
machine.setup_linux_syscalls(false, false);  // Linux ABI, no filesystem, no sockets
machine.setup_native_heap(570, heap_base, heap_size);
machine.setup_native_memory(575);
```

`setup_linux_syscalls(filesystem, sockets)` provides a Linux-compatible syscall environment. Passing `(false, false)` gives the guest a full Linux ABI (brk, mmap, clock_gettime, etc.) but no filesystem or network access.

---

## Calling Guest Functions

Calling a function in the guest is the intended host-to-guest communication channel.

### vmcall

```cpp
// By address (fastest - no symbol lookup)
auto result = machine.vmcall<MAX_INSTRUCTIONS>(function_addr, arg0, arg1);

// By name (symbol lookup each time)
auto result = machine.vmcall<MAX_INSTRUCTIONS>("my_function", arg0, arg1);
```

Arguments follow the same calling convention as host functions. Integers in A0-A7, floats in FA0-FA7. Strings and structs are pushed to the guest stack and a pointer is passed in the register.

### PreparedCall (recommended for repeated calls)

`PreparedCall` resolves the function address once and enforces a type signature:

```cpp
// Prepare once
riscv::PreparedCall<8, void(int, float), MAX_INSTRUCTIONS> pcall(machine, "on_update");

// Call many times (fast)
pcall(42, 3.14f);
pcall(43, 2.71f);
```

The type parameter `void(int, float)` enforces at compile time that callers pass the correct types. `PreparedCall` also attempts to create a fast-path that skips overhead when the guest function returns immediately.

### Preempt (nested/reentrant calls)

When a guest function is already executing (e.g., a host function handler needs to call back into the guest), use `preempt`:

```cpp
auto result = machine.preempt(MAX_INSTRUCTIONS, function_addr, arg0);
```

Preempt saves and restores the full register file and instruction counter, so the outer call resumes exactly where it left off.

### Call Depth Management

In production wrappers, track call depth to prevent recursive call loops:

```cpp
struct ScriptDepthMeter {
    ScriptDepthMeter(uint8_t& val) : m_val(++val) {}
    ~ScriptDepthMeter() { m_val--; }
    uint8_t get() const noexcept { return m_val; }
    bool is_one() const noexcept { return m_val == 1; }
    uint8_t& m_val;
};

// In call wrapper:
ScriptDepthMeter meter(m_call_depth);
if (meter.is_one())
    return machine.vmcall<MAX>(addr, args...);     // Direct call (fastest)
else if (meter.get() < MAX_DEPTH)
    return machine.preempt(MAX, addr, args...);    // Nested call
else
    throw std::runtime_error("Max call depth exceeded");
```

Depth 1 uses `vmcall` (fastest, owns CPU state). Depth 2+ uses `preempt` (saves/restores caller state). Depth >= MAX rejects the call.

---

## Heap Takeover

To pass complex types (strings, vectors) between host and guest, the host must control the guest's heap. This is done through native helper syscalls.

### Setup (Host Side)

```cpp
// Allocate a heap region in guest memory
gaddr_t heap_base = machine.memory.mmap_allocate(MAX_HEAP);

// Install native heap management (malloc/free/etc. as syscalls)
machine.setup_native_heap(HEAP_SYSCALLS_BASE, heap_base, MAX_HEAP);

// Install native memory operations (memcpy/memset/etc. as syscalls)
machine.setup_native_memory(MEMORY_SYSCALLS_BASE);
```

This installs syscall handlers at consecutive numbers:
- `HEAP_BASE+0`: malloc
- `HEAP_BASE+1`: calloc
- `HEAP_BASE+2`: realloc
- `HEAP_BASE+3`: free
- `MEMORY_BASE+0`: memcpy
- `MEMORY_BASE+1`: memset
- etc.

### Override (Guest Side)

The guest program must override `malloc`, `free`, `memcpy`, etc. to use these syscalls instead of the default implementations. With newlib (used by `riscv64-unknown-elf-g++`), this requires two pieces:

**1. Linker flags** using `--wrap` to redirect calls:

```cmake
# In CMakeLists.txt for the guest binary
target_link_libraries(${NAME} "-Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc")
target_link_libraries(${NAME} "-Wl,--wrap=memcpy,--wrap=memset,--wrap=memcmp,--wrap=memmove")
target_link_libraries(${NAME} "-Wl,--wrap=strlen,--wrap=strcmp,--wrap=strncmp")
```

The `--wrap=malloc` flag causes the linker to resolve `malloc` → `__wrap_malloc`, and the original becomes `__real_malloc`.

**2. Assembly stubs** in a guest source file (e.g., `env.cpp`). For malloc/free/calloc/realloc, pure asm stubs are sufficient:

```cpp
#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)

#define HEAP_SYSCALLS_BASE  490
#define SYSCALL_MALLOC  (HEAP_SYSCALLS_BASE+0)
#define SYSCALL_FREE    (HEAP_SYSCALLS_BASE+3)

#define GENERATE_SYSCALL_WRAPPER(name, number) \
    asm(".global " #name "\n" #name ":\n  li a7, " STRINGIFY(number) "\n  ecall\n  ret\n");

asm(".pushsection .text, \"ax\", @progbits\n");
GENERATE_SYSCALL_WRAPPER(__wrap_malloc,  SYSCALL_MALLOC);
GENERATE_SYSCALL_WRAPPER(__wrap_free,    SYSCALL_FREE);
// ... similarly for calloc, realloc
asm(".popsection\n");
```

For memcpy/memset, inline asm with proper clobber constraints is needed so the compiler can reason about memory effects:

```cpp
#define MEMORY_SYSCALLS_BASE 495
#define memcpy __wrap_memcpy

extern "C" void* memcpy(void* vdest, const void* vsrc, size_t size)
{
    register char*       a0 asm("a0") = (char*)vdest;
    register const char* a1 asm("a1") = (const char*)vsrc;
    register size_t      a2 asm("a2") = size;
    register long syscall_id asm("a7") = MEMORY_SYSCALLS_BASE + 0;

    asm volatile ("ecall"
    :  "=m"(*(char(*)[size]) a0)
    :  "r"(a0),
       "r"(a1), "m"(*(const char(*)[size]) a1),
       "r"(a2), "r"(syscall_id));
    return vdest;
}
```

See the `examples/gamedev/cpp_program/env.cpp` file for the complete set of overrides.

**Verification**: Use `objdump -d program.elf | grep -A4 '__wrap_malloc'` to confirm the override is a 3-instruction syscall stub (`li a7, N; ecall; ret`). If you see a large function body instead, the takeover failed.

### Host Allocation on Behalf of Guest

Once the heap is taken over, the host can allocate and free guest memory:

```cpp
gaddr_t ptr = machine.arena().malloc(1024);     // Allocate 1024 bytes
machine.arena().free(ptr);                       // Free it

// Scoped allocation (RAII) — wraps a typed object on the guest heap
riscv::ScopedArenaObject<8, MyStruct> obj(machine, /* constructor args */);
gaddr_t addr = obj.address();  // Freed on scope exit
```

### Guest Ownership of Host-Allocated Memory

Arena allocations live in guest memory. The guest can take ownership of data the host allocates — this is fundamental to how complex types work. The host allocates on the guest heap via `arena().malloc()`, but the memory is guest-visible and guest-writable. The guest can move from it, store it, and use it long after the host call returns.

Example: the host allocates a `std::string` on the guest heap, passes it to the guest, the guest moves it into a static variable, and a later call proves the guest still owns it:

```cpp
// Guest side:
static std::string stored_string;

PUBLIC(void take_string(std::string& s))
{
    stored_string = std::move(s);  // Guest takes ownership
}

PUBLIC(void print_stored())
{
    printf("stored: %s\n", stored_string.c_str());  // Still valid
}
```

```cpp
// Host side:
using ScopedStr = riscv::ScopedArenaObject<8, riscv::GuestStdString<8>>;

// String must exceed SSO (>15 bytes) so the move transfers a heap pointer
ScopedStr str(machine, "This string is heap-allocated and guest-owned!");
assert(!str->empty());  // Host sees the string

Event<void(ScopedStr&)> take_string(script, "take_string");
take_string(str);       // Guest moves it into a static variable

assert(str->empty());   // Host confirms: source is now empty (moved-from)

// The ScopedStr destructor frees the GuestStdString struct (now empty),
// but NOT the string data — the guest's static variable owns that.

Event<void()> print_stored(script, "print_stored");
print_stored();         // Guest prints the string it owns — still valid
```

This works because `ScopedArenaObject`'s destructor calls `GuestStdString::free()`, which checks the size field. A moved-from string has `size == 0`, so the free is a no-op for the data buffer. The struct allocation itself is freed, but the string data that the guest moved into its own variable remains on the arena heap, alive and owned by the guest.

---

## Guest Datatypes

With heap takeover, the host can construct and pass C++ standard library types that the guest understands.

### Type Aliases (typical project setup)

```cpp
using CppString = riscv::GuestStdString<8>;                  // Mirrors libstdc++ std::string
template <typename T>
using CppVector = riscv::GuestStdVector<8, T>;               // Mirrors std::vector<T>
using CppStringVector = CppVector<CppString>;                // std::vector<std::string>
```

### GuestStdString

Mirrors the libstdc++ `std::string` memory layout, including Small String Optimization (SSO). Strings <= 15 bytes are stored inline. Longer strings are allocated on the guest heap via the arena.

```cpp
// Read a string from guest memory (in a host function handler)
CppString* guest_str = ...;  // Pointer into guest memory
std::string host_str = guest_str->to_string(machine);      // Copy to host
std::string_view view = guest_str->to_view(machine);       // Zero-copy view

// Check if empty without copying
bool empty = guest_str->empty();

// Create a string for the guest
CppString str;
str.set_string(machine, self_address, "Hello from host");

// Free when done
str.free(machine);
```

### GuestStdVector

Mirrors `std::vector<T>` layout (begin/end/capacity pointers).

```cpp
// Read a vector from guest memory
CppVector<int>* guest_vec = ...;
std::span<int> span = guest_vec->to_span(machine);        // Zero-copy span
std::vector<int> host_vec = guest_vec->to_vector(machine); // Copy to host

// Create a vector for the guest
CppVector<int> vec(machine, {1, 2, 3, 4, 5});

// Vector of strings
CppStringVector strvec(machine, {"hello", "world"});

// Free (recursively frees contained strings too)
strvec.free(machine);
vec.free(machine);
```

### ScopedArenaObject (RAII wrapper)

```cpp
// Automatically allocated on guest heap, freed on scope exit
riscv::ScopedArenaObject<8, CppString> str(machine, "test");
gaddr_t guest_addr = str.address();    // Address in guest memory
auto view = str->to_view(machine);     // operator-> accesses the GuestStdString
auto& obj = *str;                      // operator* dereferences it

// Pass to a vmcall - guest sees a valid std::string at that address
machine.vmcall<MAX>(func, str);        // passes str.address() automatically
// str freed automatically when it goes out of scope
```

### Passing Complex Types via vmcall

**String arguments** have two distinct modes depending on what the guest function expects:

| Host Argument Type | What vmcall Does | Guest Receives |
|---|---|---|
| `std::string` or `const char*` | Pushes null-terminated C string to guest stack, passes pointer in register | `const char*` |
| `GuestStdString<W>` | SSO-adjusts and pushes full string object to guest stack | `const std::string&` |
| `ScopedArenaObject<W, GuestStdString<W>>&` | Passes the guest heap address directly | `const std::string&` |

**Common mistake**: Passing `std::string("hello")` via vmcall and having the guest receive it as `const std::string&`. This silently produces garbage because the guest gets a C string pointer where it expects a `std::string` object layout. If the guest function takes `const std::string&`, use `ScopedArenaObject<W, GuestStdString<W>>` on the host side.

```cpp
// Pattern 1: Guest expects const char* — just pass a string
Event<void(std::string)> greet(script, "greet");
greet("World");  // Guest: void greet(const char* name)

// Pattern 2: Guest expects const std::string& — use ScopedArenaObject
using ScopedStr = riscv::ScopedArenaObject<8, riscv::GuestStdString<8>>;
ScopedStr str(machine, "World");
Event<void(ScopedStr&)> greet_str(script, "greet_str");
greet_str(str);  // Guest: void greet_str(const std::string& name)
```

For passing larger structures, allocate on the guest heap, fill them in, then pass the address:

```cpp
auto addr = machine.arena().malloc(sizeof(MyStruct));
auto* obj = machine.memory.memarray<MyStruct>(addr, 1);
obj->field1 = 42;
obj->field2 = 3.14f;
machine.vmcall<MAX>(func, addr);
machine.arena().free(addr);
```

---

## Generated Host Functions

In production, host functions are not registered by syscall number. Instead, both guest and host share a JSON definition of every callable function. A code generator (`generate.py`) produces guest-side assembly stubs and a `dyncall_table` embedded in the guest's rodata. At load time, the host reads this table, matches each entry by CRC32 hash of the function signature, and builds a flat dispatch array. If the guest asks for a function the host doesn't have, it fails immediately at init, instead of at runtime.

This is the proper way to handle host functions. The guest and host "compare notes" at initialization: the guest declares what it needs (by signature hash), and the host confirms it can provide each one. A version mismatch or missing function is caught before any gameplay logic runs.

### Overview

The system has four components that must agree:

```
host_functions.json   (shared definition, single source of truth)
        |
        v
  generate.py         (produces guest stubs + dyncall_table)
        |
        v
  guest binary        (embeds dyncall_table in .rodata, calls stubs as C functions)
        |
        v
  host resolution     (reads dyncall_table, matches CRC32 hashes, builds dispatch array)
```

The CRC32 hash of each function's **signature string** is the key that ties everything together. Both the Python generator and the C++ host must normalize whitespace and use the same CRC32 polynomial (`0xEDB88320`, standard CRC32) for hashes to match.

### Step 1: Define Functions in JSON

Create a `host_functions.json` that lists every callable function with its C signature:

```json
{
  "typedef": [
    "typedef void (*timer_callback) (int, void*)"
  ],
  "initialization": ["Block::new", "Recipe::new"],
  "clientside": ["GUI::open"],
  "serverside": ["Entity::spawn"],

  "Timer::periodic": "int sys_timer_periodic (float, float, timer_callback, void*, size_t)",
  "Block::new": "unsigned sys_block_new (const char*, const char*)",
  "Recipe::new": "unsigned sys_recipe_new (const char*, size_t)",
  "GUI::open": "unsigned sys_gui_open (unsigned, gui_close_callback, void*, size_t)",
  "Entity::spawn": "uint32_t sys_entity_spawn (const char*, double, double, double, float, float, float)",
  "Game::get_time": "double sys_game_get_time ()"
}
```

**Special keys** (metadata, not functions):
- `typedef`: Type aliases emitted into the generated header.
- `initialization`: Names of functions that should only be callable during init (phase 1).
- `clientside` / `serverside`: Names of functions restricted to one side.

**Everything else** is a `"Name": "C signature"` pair. The signature string is the function's identity — it is what gets CRC32-hashed. The `sys_` prefix on the function name is convention, not requirement.

### Step 2: Generate Guest Stubs

The `generate.py` script (shipped in the libriscv repository) reads the JSON and produces:

1. **A C header** (`host_functions.h`) with `extern` prototypes and any typedefs.
2. **A C source** (`host_functions.c`) with assembly stubs and the `dyncall_table`.

The guest program uses these by including the header and calling the functions as normal C calls. The generated `.c` must be compiled into the guest binary.

Here is a fully working host-function generator:
```py
import binascii
import json
from array import array

from argparse import ArgumentParser
parser = ArgumentParser()
parser.add_argument("-j", "--json", dest="jsonfile", default="dyncalls.json",
                    help="read JSON from FILE", metavar="FILE")
parser.add_argument("-o", "--output", dest="output", required=True,
                    help="write generated prototypes to FILE", metavar="FILE")
parser.add_argument("-v", "--verbose",
                    action="store_true", dest="verbose", default=False,
                    help="print status messages to stdout")
parser.add_argument('--dyncall', dest='dyncall', action='store', default=504,
                   help='set the dyncall system call number')

args = parser.parse_args()

# Use system call number 504 (instead of custom instruction)
use_syscall = False
# this is the plain CRC32 polynomial
poly = 0xEDB88320
# we need to be able to calculate CRC32 using any poly
table = array('L')
for byte in range(256):
    crc = 0
    for bit in range(8):
        if (byte ^ crc) & 1:
            crc = (crc >> 1) ^ poly
        else:
            crc >>= 1
        byte >>= 1
    table.append(crc)

def crc32(string):
    value = 0xffffffff
    for ch in string:
        value = table[(ord(ch) ^ value) & 0xff] ^ (value >> 8)

    return -1 - value

def is_type(string):
	keywords = ["unsigned", "char", "short", "int", "long", "float", "double", "size_t", "int8_t", "uint8_t", "int16_t", "uint16_t", "int32_t", "uint32_t", "int64_t", "uint64_t"]
	conventions = ("_callback", "_t")
	return ("*" in string) or string in keywords or string.endswith(conventions)

def find_arguments(string):
	sfront = string.split('(', 1)
	retval = [sfront[0].split(' ')[0]]
	strargs = sfront[1].split(')')[0]
	# [retval, arg0, arg1, '']
	fargs = retval + strargs.split(", ")
	# Remove parameter names
	for (idx, arg) in enumerate(fargs):
		symbols = arg.split(" ")
		if len(symbols) > 1:
			last = symbols[-1]
			if not is_type(last):
				symbols.pop()
				fargs[idx] = " ".join(symbols)
	# Remove empty argument lists
	if fargs[-1] == "":
		fargs.pop()
	return fargs

# load JSON
j = {}
with open(args.jsonfile) as f:
	j = json.load(f)

# List of client-side only dyncalls
client_side = []
if "clientside" in j:
	for key in j["clientside"]:
		client_side.append(key)

# List of server-side only dyncalls
server_side = []
if "serverside" in j:
	for key in j["serverside"]:
		server_side.append(key)

# List of initialization-only dyncalls
initialization = []
if "initialization" in j:
	for key in j["initialization"]:
		initialization.append(key)

header = """
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

""";

# iterate typedefs first
for key in j:
	if key == "typedef":
		for typedef in j[key]:
			header += typedef + ";\n"
header += "\n"

source = '__asm__(".section .text\\n");\n\n'
dyncallindex = 0

dyncall = ""

# create dyncall prototypes and assembly
for key in j:
	if key == "typedef" or key == "clientside" or key == "serverside" or key == "initialization":
		continue
	else:
		asmdef  = " ".join(j[key].split())
		asmname = asmdef.split(' ')[1]

		fargs = find_arguments(asmdef)

		crcval = crc32(asmdef) & 0xffffffff
		crc = '%08x' % crcval

		header += "// " + key + ": 0x" + crc + "\n"
		header += "extern " + asmdef + ";\n"

		if args.verbose:
			print("Dynamic call: " + key + ", hash 0x" + crc)

		# Each dynamic call has a table index where the name and hash is stored
		dyncall += '  .long 0x' + crc + '\\n\\\n'
		dyncall += '  .long ' + str(0) + '\\n\\\n'
		dyncall += '  .long ' + asmname + '_str\\n\\\n'

		# Flags (one byte each for client-side, server-side, initialization, and padding)
		is_client_side = key in client_side
		is_server_side = key in server_side
		is_initialization = key in initialization
		dyncall += '  .byte ' + str(int(is_initialization)) + '\\n\\\n'
		dyncall += '  .byte ' + str(int(is_client_side)) + '\\n\\\n'
		dyncall += '  .byte ' + str(int(is_server_side)) + '\\n\\\n'
		dyncall += '  .byte 0\\n\\\n'

		# These dynamic calls use the table indexed variant
		# Each dynamic call has a table index where the name and hash is stored
		# and at run-time this value is lazily resolved
		source += '__asm__("\\n\\\n'
		source += '.global ' + asmname + '\\n\\\n'
		source += '.func ' + asmname + '\\n\\\n'
		source += asmname + ':\\n\\\n'
		if use_syscall:
			source += '  li t0, ' + str(dyncallindex) + '\\n\\\n'
			source += '  li a7, ' + str(args.dyncall) + '\\n\\\n'
			source += '  ecall\\n\\\n'
		else:
			source += '  .insn i 0b1011011, 0, x0, x0, ' + str(dyncallindex) + '\\n\\\n'
		source += '  ret\\n\\\n'
		source += '.endfunc\\n\\\n'
		source += '.pushsection .rodata\\n\\\n'
		source += asmname + '_str:\\n\\\n'
		source += '.asciz \\\"' + key + '\\\"\\n\\\n'
		source += '.popsection\\n\\\n'
		source += '");\n\n'

		dyncallindex += 1

header += """
#ifdef __cplusplus
}
#endif
"""

dyncall_header =  '__asm__("\\n\\\n'
dyncall_header += '.pushsection .rodata\\n\\\n'
dyncall_header += '.align 8\\n\\\n'
dyncall_header += '.global dyncall_table\\n\\\n'
dyncall_header += 'dyncall_table:\\n\\\n'
dyncall_header += '  .long ' + str(dyncallindex) + '\\n\\\n'

dyncall = dyncall_header + dyncall
dyncall += '.popsection\\n\\\n'
dyncall += '");\n\n'

source += dyncall

if (args.verbose):
	print("* There are " + str(dyncallindex) + " dynamic calls")

with open(args.output + ".h", "w") as hdrfile:
	hdrfile.write(header)
with open(args.output + ".c", "w") as srcfile:
	srcfile.write(source)
```

**CMake integration for the guest build:**

```cmake
# Path to the shared JSON and the generator script
set(JSON_FILE "${CMAKE_SOURCE_DIR}/host_functions.json")
set(PYPROGRAM "${CMAKE_SOURCE_DIR}/generate.py")
set(GEN_DIR   "${CMAKE_BINARY_DIR}/dyncalls")
set(GEN_FILES "${GEN_DIR}/host_functions")

# Generate .h and .c from JSON
file(MAKE_DIRECTORY ${GEN_DIR})
add_custom_command(
    OUTPUT ${GEN_FILES}.h ${GEN_FILES}.c
    COMMAND python3 ${PYPROGRAM} --verbose -j ${JSON_FILE} -o ${GEN_FILES}
    DEPENDS ${PYPROGRAM} ${JSON_FILE}
)
add_custom_target(generate_dyncalls ALL DEPENDS ${GEN_FILES}.h ${GEN_FILES}.c)

# Mark generated files so CMake doesn't complain about missing sources
set_source_files_properties(${GEN_FILES}.h ${GEN_FILES}.c PROPERTIES GENERATED TRUE)

# Add to guest binary (assuming add_guest_binary is defined as shown earlier)
add_guest_binary(guest program.cpp ${GEN_FILES}.c)
add_dependencies(guest generate_dyncalls)
target_include_directories(guest PRIVATE ${GEN_DIR})

# The dyncall_table symbol in rodata can be garbage-collected by the linker.
# This flag prevents that:
target_link_libraries(guest "-Wl,--undefined=dyncall_table")
```

After building, the guest code uses the generated functions like any C API:

```cpp
// Guest program.cpp
#include "host_functions.h"

PUBLIC(void on_init())
{
    sys_block_new("dirt", "terrain/dirt");
    sys_recipe_new("pickaxe", 7);
}

PUBLIC(void on_update())
{
    double t = sys_game_get_time();
    // ...
}
```

#### What the Generator Produces

Each function becomes a 2-instruction assembly stub — a custom RISC-V instruction encoding the table index, then `ret`:

```asm
.global sys_timer_periodic
sys_timer_periodic:
  .insn i 0b1011011, 0, x0, x0, 0    # table index 0
  ret
```

The `.insn i 0b1011011` is a custom RISC-V instruction. The emulator does not recognize it as a standard instruction, so it traps to a host callback. The table index is encoded in the 12-bit immediate field (supporting up to 2048 host functions). This is faster than ECALL because the index is in the instruction itself — no register setup, no syscall dispatch table.

The generator also emits a `dyncall_table` symbol in the guest's `.rodata` section. This table is what the host reads at init to match functions:

```
dyncall_table:
  .long <count>                    # Number of entries
  # Entry 0:
  .long 0xABCD1234                 # CRC32 of "int sys_timer_periodic (float, float, ...)"
  .long 0                          # Reserved
  .long sys_timer_periodic_str     # Pointer to name string in rodata ("Timer::periodic")
  .byte 0                          # initialization_only flag
  .byte 0                          # client_side_only flag
  .byte 0                          # server_side_only flag
  .byte 0                          # padding
  # Entry 1: ...
```

Each entry is 16 bytes. The `strname` field is a guest-space address pointing to a null-terminated string in `.rodata` — it is not a host pointer. The host reads it with `machine().memory.memstring(entry.strname)`.

### Step 3: Host-Side Registration

The host registers handlers by name and signature. The signature is CRC32-hashed to match against the guest's table.

**Required include:**

```cpp
#include <libriscv/util/crc32.hpp>
```

**The host function registry and registration:**

```cpp
using ghandler_t = std::function<void(Script&)>;

struct HostFunction {
    std::string name;
    std::string signature;
    ghandler_t  func;
};

// Static map shared across all Script instances: CRC32(signature) -> handler
static std::map<uint32_t, HostFunction> m_host_functions;
```

**Whitespace normalization** is critical. The generator normalizes signatures to single-spaced strings before hashing (`" ".join(s.split())` in Python). The host must do the same, or hashes won't match:

```cpp
static std::string single_spaced(std::string s) {
    size_t pos = 0;
    while ((pos = s.find("  ", pos)) != std::string::npos)
        s.replace(pos, 2, " ");
    return s;
}

void Script::set_host_function(
    std::string name, std::string signature, ghandler_t handler)
{
    signature = single_spaced(signature);
    const uint32_t hash = riscv::crc32(signature.c_str());

    auto it = m_host_functions.find(hash);
    if (it != m_host_functions.end() && it->second.name != name)
        throw std::runtime_error("CRC32 hash collision: " + name + " vs " + it->second.name);

    m_host_functions[hash] = {std::move(name), std::move(signature), std::move(handler)};
}
```

**Registration** — the signature string must match the JSON (whitespace is normalized, so minor spacing differences are OK):

```cpp
Script::set_host_function(
    "Timer::periodic",
    "int sys_timer_periodic (float, float, timer_callback, void*, size_t)",
    [](Script& script) {
        auto& machine = script.machine();
        auto [interval, duration, callback, data, size] =
            machine.sysargs<float, float, gaddr_t, gaddr_t, gaddr_t>();
        // ... create timer ...
        machine.set_result(timer_id);
    });

Script::set_host_function(
    "Game::get_time",
    "double sys_game_get_time ()",
    [](Script& script) {
        script.machine().set_result(game_time());
    });
```

### Step 4: Resolution at Init

When a guest program is loaded, the host reads the `dyncall_table` from guest rodata, looks up each entry by CRC32 hash, and builds a flat dispatch array. This is called once (or twice for two-phase init).

**The table entry layout** must match what `generate.py` emits (16 bytes per entry):

```cpp
struct HostFunctionDesc {
    uint32_t hash;
    uint32_t reserved;
    uint32_t strname;       // Guest-space address of the name string in .rodata
    bool initialization_only;
    bool client_side_only;
    bool server_side_only;
    bool padding;
};
static_assert(sizeof(HostFunctionDesc) == 16);
```

**The dispatch array** is a `std::vector` on the Script instance:

```cpp
// In script.hpp, add to the Script class:
std::vector<ghandler_t> m_host_function_array;
gaddr_t m_g_host_function_table = 0;

static std::size_t host_function_count() noexcept {
    return m_host_functions.size();
}
```

**Resolution logic:**

```cpp
void Script::resolve_host_functions(bool initialization, bool client_side)
{
    // Find the dyncall_table symbol in the guest ELF
    m_g_host_function_table = machine().address_of("dyncall_table");
    if (m_g_host_function_table == 0x0)
        throw std::runtime_error(name() + ": dyncall_table not found in guest ELF");

    // First 4 bytes are the entry count
    const uint32_t count = machine().memory.read<uint32_t>(m_g_host_function_table);
    if (count > 2048)
        throw std::runtime_error(name() + ": dyncall_table has bogus entry count");

    // View the entries directly from guest rodata (zero-copy, flat arena)
    auto entries = machine().memory.memspan<const HostFunctionDesc>(
        m_g_host_function_table + 4, count);

    m_host_function_array.clear();
    m_host_function_array.reserve(count);
    std::size_t unimplemented = 0;

    for (unsigned i = 0; i < count; i++) {
        auto& entry = entries[i];

        // Phase restriction: init-only functions get stubbed out after init
        if (entry.initialization_only && !initialization) {
            m_host_function_array.push_back([](auto&) {
                throw std::runtime_error("Init-only host function called at runtime");
            });
            continue;
        }
        // Client/server restrictions
        if (entry.client_side_only && !client_side) {
            m_host_function_array.push_back([](auto&) {
                throw std::runtime_error("Client-only host function called on server");
            });
            continue;
        }
        if (entry.server_side_only && client_side) {
            m_host_function_array.push_back([](auto&) {
                throw std::runtime_error("Server-only host function called on client");
            });
            continue;
        }

        // Match by CRC32 hash
        auto it = m_host_functions.find(entry.hash);
        if (it != m_host_functions.end()) {
            m_host_function_array.push_back(it->second.func);
        } else {
            // Guest wants a function the host doesn't have
            auto func_name = machine().memory.memstring(entry.strname);
            fprintf(stderr, "WARNING: Unimplemented host function '%s' (hash %08x)\n",
                func_name.c_str(), entry.hash);
            m_host_function_array.push_back([](auto&) {
                throw std::runtime_error("Unimplemented host function");
            });
            unimplemented++;
        }
    }

    fprintf(stderr, "Resolved %u host functions for '%s' (%zu unimplemented)\n",
        count, name().c_str(), unimplemented);
}
```

`memstring(addr)` reads a null-terminated string from guest memory at the given guest-space address. The `strname` field in each entry points to a name string that `generate.py` placed in `.rodata` alongside the stubs.

### Step 5: Custom Instruction Dispatch

The host must intercept the custom instruction (opcode `0b1011011`) and dispatch by index into the flat array. This is set up **once**, before any machine is created, as a static callback:

```cpp
#include <libriscv/rv32i_instr.hpp>

using Instruction = riscv::Instruction<Script::MARCH>;
using CPU = riscv::CPU<Script::MARCH>;

// The handler that runs when the custom instruction executes.
// It reads the table index from the instruction's immediate field
// and calls the corresponding host function.
static const Instruction unchecked_hostcall {
    [](CPU& cpu, riscv::rv32i_instruction instr) {
        auto& script = *cpu.machine().template get_userdata<Script>();
        script.m_host_function_array[instr.Itype.imm](script);
    }, nullptr};

// Called once at startup, before any Script is created
void Script::setup_dispatch() {
    CPU::on_unimplemented_instruction
        = [](riscv::rv32i_instruction instr) -> const Instruction& {
        // Match opcode 0b1011011 with rs1=0, rd=0 (our custom encoding)
        if (instr.opcode() == 0b1011011
            && instr.Itype.rs1 == 0
            && instr.Itype.rd == 0)
        {
            if ((unsigned)instr.Itype.imm < Script::host_function_count())
                return unchecked_hostcall;
        }
        return CPU::get_unimplemented_instruction();
    };
}
```

`on_unimplemented_instruction` is a **static** callback on `CPU<W>`. It is called whenever the emulator encounters an instruction it doesn't recognize. It returns a reference to an `Instruction` object whose handler function the emulator then executes. This callback is shared across all Machine instances of the same width — set it once at program startup.

The dispatch path at runtime: guest executes the custom instruction → emulator doesn't recognize the opcode → calls `on_unimplemented_instruction` → returns `unchecked_hostcall` → handler reads `instr.Itype.imm` as the array index → calls `m_host_function_array[index]`. After the first encounter, the emulator caches the instruction handler, so subsequent calls to the same guest function skip the `on_unimplemented_instruction` callback entirely.

### Two-Phase Initialization

Call `resolve_host_functions` twice: first with `initialization=true` (all functions available), then after calling the guest's init function, re-resolve with `initialization=false`. Init-only entries (like `Block::new`) are replaced with error-throwing stubs, enforcing the boundary between setup and runtime:

```cpp
void Script::second_stage_init(bool is_client_side) {
    resolve_host_functions(/*initialization=*/true, is_client_side);
    this->call(m_on_init_addr);  // Guest registers blocks, items, etc.
    resolve_host_functions(/*initialization=*/false, is_client_side);
}
```

### Why This Matters

- **Mismatch detection**: If the guest was compiled against a newer API than the host provides, you get a clear list of every missing function at init — not a crash during gameplay.
- **Signature safety**: The CRC32 hash covers the full C signature. If the guest expects `int sys_foo(float)` but the host registered `int sys_foo(int)`, the hashes won't match and resolution fails.
- **Per-environment filtering**: Client-only and server-only flags let the same guest binary run on both client and server, with unavailable functions stubbed out.
- **Zero dispatch overhead**: After resolution, every host call is a fast flat array index lookup.

---

## Advanced: Zero-Copy Structures

With the flat arena, you can view complex guest structures directly without copying.

### Guest-Side Structure

The guest defines a `Dialogue` struct using normal C++ types (`std::string`, `std::vector`):

```cpp
// Guest code
struct Dialogue {
    std::string name;
    std::string portrait;
    std::string voice;
    std::vector<DialogueText> texts;
    bool cancellable;
    std::vector<DialogueChoice> choices;
    // ...
};

// Guest passes it to host
sys_npc_do_dialogue(entity_uid, &dialogue, sizeof(dialogue));
```

### Host-Side Zero-Copy View

The host defines a mirror struct using `GuestStdString` and `GuestStdVector`:

```cpp
// Host code - mirrors the guest struct layout exactly
struct GuestDialogue {
    CppString name;
    CppString portrait;
    CppString voice;
    CppVector<GuestText> texts;
    bool cancellable;
    CppVector<GuestChoice> choices;
    // ... more fields
};
```

In the host function handler:

```cpp
auto [uid, g_view, g_size] = script.machine().sysargs<uint32_t, GuestDialogue*, gaddr_t>();

// Validate size matches
if (g_size != sizeof(GuestDialogue)) { /* error */ }

// Direct access - no copy, no serialization
auto& guestDialogue = *g_view;

// View strings zero-copy
std::string_view name = guestDialogue.name.to_view(machine);

// Iterate over guest vector zero-copy
std::span<const GuestText> texts = guestDialogue.texts.to_span(machine);
for (auto& text : texts) {
    std::string content = text.text.to_string(machine);
    // Process text...
}

// Access nested vectors
for (auto& choice : guestDialogue.choices.to_span(machine)) {
    std::string choiceText = choice.text.to_string(machine);
    // Process choice...
}
```

The `GuestDialogue*` returned by `sysargs` is a direct pointer into guest memory (flat arena). The `CppString` and `CppVector` fields are read in-place. String data is accessed through `to_string()` (copy) or `to_view()` (zero-copy view). Vector elements are accessed through `to_span()` (zero-copy span).

**This only works with the flat arena.** With virtual paging, the structure might span page boundaries and the pointer would be invalid.

### Creating Guest Events from Host-Viewed Data

The dialogue handler also demonstrates creating `Event` objects from guest function addresses found in the viewed structure:

```cpp
struct GuestDialogueEvent {
    gaddr_t address;         // Guest function pointer
    CppStringVector args;    // Arguments to pass
};

// In handler:
if (guestDialogue.nextScriptFunction.address) {
    Event<void(int)> event(script, guestDialogue.nextScriptFunction.address);
    // This Event can now be called later to trigger the next dialogue step
}
```

---

## Production Integration Example

Here we will demonstrate a complete production integration. Key components:

### Script Wrapper

A `Script` class wraps `Machine<8>` and adds:
- **Call depth tracking** via `ScriptDepthMeter` (max 8 levels)
- **Scoped calls** that set a `Scope*` context pointer for the duration
- **Guest heap allocation** via `guest_alloc()` / `guest_free()`
- **Per-thread forking** via `get_fork()` for thread safety
- **Host function resolution** with init/client/server filtering
- **Error handling** with backtraces and optional GDB remote debugging

### Event System

`Event<F, Usage>` wraps a `PreparedCall` with a type-enforced function signature:

```cpp
// Create an event pointing to a guest function
Event<void(uint32_t, float)> on_damage("game_script", "on_entity_damage");

// Call it - type-checked at compile time
on_damage.call(entity_uid, damage_amount);
```

`Event` supports two usage patterns:
- `SharedScript`: Always uses the same script instance
- `PerThread`: Automatically selects the current thread's fork

### Machine Configuration

```cpp
auto options = riscv::MachineOptions<8>{
    .memory_max = 28ULL << 20,   // 28 MB
    .stack_size = 1ULL << 20,    // 1 MB (0x100000)
    .default_exit_function = "fast_exit",
    // JIT disabled for production sandboxing
    .translate_enabled = false,
};

machine = std::make_unique<machine_t>(binary, options);
machine->set_userdata<Script>(this);

// Setup syscalls and native helpers
machine->setup_argv({name()});
machine->setup_newlib_syscalls();

// Heap takeover
gaddr_t heap_base = machine->memory.mmap_allocate(MAX_HEAP);
machine->setup_native_heap(570, heap_base, MAX_HEAP);
machine->setup_native_memory(575);
machine->setup_native_threads(590);
machine->arena().set_max_chunks(32000);
```

### Boot Sequence

```
1. Construct Machine with ELF binary + options
2. Install syscall handlers (api_write, api_dyncall, api_game_on_init, etc.)
3. Install native heap/memory/threads
4. machine.simulate(MAX_BOOT_INSTR)     -- runs main() until it pauses
5. resolve_host_functions(init=true)    -- all host functions available
6. call(on_init_addr)                   -- guest registers blocks, items, NPCs
7. resolve_host_functions(init=false)   -- lock out init-only functions
8. Machine is ready for vmcalls
```

### Request Handling

```
Host event occurs (player clicks, timer fires, NPC update, etc.)
  -> Look up Event<F> for this event type
  -> event.call(args...)
    -> ScriptDepthMeter increments depth
    -> If depth == 1: machine.vmcall<MAX>(addr, args)
    -> If depth > 1: machine.preempt(MAX, addr, args)
    -> Guest function executes
      -> May call host functions (HOSTCALL instruction)
        -> Host handler reads args via sysargs<>
        -> Host handler may call back into guest (depth increases)
        -> Host handler sets result via set_result()
      -> Guest function returns
    -> ScriptDepthMeter decrements depth
  -> Return value available
```

### Thread Safety

Each thread gets its own fork of the script via `Script::get_fork()`. Forks share the read-only binary and host function table but have independent registers, stack, and heap. This avoids locking during execution.

```cpp
// Automatically creates or reuses a fork for the current thread
Script& fork = original_script.get_fork();
fork.call("on_update", delta_time);
```

---

## Minimal Script Wrapper (End-to-End)

This section shows a complete, minimal Script wrapper that ties together flat arena, heap takeover, call depth tracking, and the Event system. Use this as a starting point.

### Script Header (script.hpp)

```cpp
#pragma once
#include <libriscv/machine.hpp>
#include <libriscv/native_heap.hpp>
#include <optional>
#include <unordered_map>

struct Script {
    static constexpr int MARCH = 8; // 64-bit

    using gaddr_t   = riscv::address_type<MARCH>;
    using sgaddr_t  = riscv::signed_address_type<MARCH>;
    using machine_t = riscv::Machine<MARCH>;

    static constexpr gaddr_t  MAX_MEMORY     = 16ULL << 20;
    static constexpr gaddr_t  STACK_SIZE     = 2ULL << 20;
    static constexpr gaddr_t  MAX_HEAP       = 8ULL << 20;
    static constexpr uint64_t MAX_BOOT_INSTR = 32'000'000ULL;
    static constexpr uint64_t MAX_CALL_INSTR = 32'000'000ULL;
    static constexpr uint8_t  MAX_CALL_DEPTH = 8;

    static constexpr int HEAP_SYSCALLS_BASE   = 490;
    static constexpr int MEMORY_SYSCALLS_BASE = 495;

    Script(const std::string& name, const std::string& filename);

    // Call a guest function by name or address, with depth tracking
    template <typename... Args>
    std::optional<sgaddr_t> call(const std::string& func, Args&&... args);
    template <typename... Args>
    std::optional<sgaddr_t> call(gaddr_t addr, Args&&... args);

    gaddr_t address_of(const std::string& name) const;
    auto& machine() { return *m_machine; }
    const auto& machine() const { return *m_machine; }
    const auto& name() const noexcept { return m_name; }

    gaddr_t guest_alloc(gaddr_t bytes) { return machine().arena().malloc(bytes); }
    bool guest_free(gaddr_t addr) { return machine().arena().free(addr) == 0x0; }

private:
    void machine_setup();
    static void setup_syscall_interface();

    std::unique_ptr<machine_t> m_machine;
    std::vector<uint8_t> m_binary;
    std::string m_name;
    gaddr_t m_heap_area = 0;
    uint8_t m_call_depth = 0;
    mutable std::unordered_map<std::string, gaddr_t> m_lookup_cache;
};

// RAII depth tracker — prevents recursive call loops
struct ScriptDepthMeter {
    ScriptDepthMeter(uint8_t& val) : m_val(++val) {}
    ~ScriptDepthMeter() { m_val--; }
    bool is_one() const noexcept { return m_val == 1; }
    uint8_t get() const noexcept { return m_val; }
private:
    uint8_t& m_val;
};

// Inline: call with depth tracking (vmcall at depth 1, preempt at 2+)
template <typename... Args>
inline std::optional<Script::sgaddr_t> Script::call(gaddr_t address, Args&&... args) {
    ScriptDepthMeter meter(this->m_call_depth);
    try {
        if (meter.is_one())
            return { machine().template vmcall<MAX_CALL_INSTR>(address, std::forward<Args>(args)...) };
        else if (meter.get() < MAX_CALL_DEPTH)
            return { machine().preempt(MAX_CALL_INSTR, address, std::forward<Args>(args)...) };
    } catch (const std::exception& e) {
        fprintf(stderr, "Script call exception: %s\n", e.what());
    }
    return std::nullopt;
}

template <typename... Args>
inline std::optional<Script::sgaddr_t> Script::call(const std::string& func, Args&&... args) {
    const auto addr = address_of(func);
    if (addr == 0x0) return std::nullopt;
    return call(addr, std::forward<Args>(args)...);
}

// Type-safe event wrapper for calling guest functions
template <typename F = void()>
struct Event {
    Event(Script& s, const std::string& func) : m_script(&s), m_addr(s.address_of(func)) {}

    template <typename... Args>
    auto operator()(Args&&... args) {
        static_assert(std::is_invocable_v<F, Args...>);
        using Ret = decltype((F*){}(args...));
        if (m_script && m_addr) {
            if (auto res = m_script->call(m_addr, std::forward<Args>(args)...)) {
                if constexpr (std::is_same_v<void, Ret>) return true;
                else return std::optional<Ret>(static_cast<Ret>(res.value()));
            }
        }
        if constexpr (std::is_same_v<void, Ret>) return false;
        else return std::optional<Ret>{std::nullopt};
    }
private:
    Script* m_script = nullptr;
    Script::gaddr_t m_addr = 0;
};
```

### Script Implementation (script.cpp)

```cpp
#include "script.hpp"
#include <fstream>

Script::Script(const std::string& name, const std::string& filename) : m_name(name) {
    std::ifstream f(filename, std::ios::binary);
    m_binary = {std::istreambuf_iterator<char>(f), {}};

    m_machine = std::make_unique<machine_t>(m_binary, riscv::MachineOptions<MARCH>{
        .memory_max = MAX_MEMORY,
        .stack_size = STACK_SIZE,
        .use_memory_arena = true,           // flat arena (default, required for zero-copy)
        .default_exit_function = "fast_exit",
    });

    static bool init = false;
    if (!init) { init = true; setup_syscall_interface(); }
    machine_setup();
    machine().setup_linux({name}, {"LC_CTYPE=C"});
    machine().simulate(MAX_BOOT_INSTR);     // runs main() until fast_exit pauses it
}

void Script::machine_setup() {
    machine().set_userdata<Script>(this);
    machine().set_printer([](const machine_t&, const char* p, size_t len) {
        printf("%.*s", (int)len, p);
    });
    m_heap_area = machine().memory.mmap_allocate(MAX_HEAP);
    machine().setup_linux_syscalls(false, false); // no fs, no sockets
    machine().setup_native_heap(HEAP_SYSCALLS_BASE, m_heap_area, MAX_HEAP);
    machine().setup_native_memory(MEMORY_SYSCALLS_BASE);
}

void Script::setup_syscall_interface() {
    // Register your custom host functions here
    machine_t::install_syscall_handler(510, [](machine_t& machine) {
        auto [name] = machine.sysargs<std::string_view>();
        printf("Host received: %.*s\n", (int)name.size(), name.data());
        machine.set_result(0);
    });
}

Script::gaddr_t Script::address_of(const std::string& name) const {
    auto it = m_lookup_cache.find(name);
    if (it != m_lookup_cache.end()) return it->second;
    auto addr = machine().address_of(name.c_str());
    m_lookup_cache.try_emplace(name, addr);
    return addr;
}
```

### Host main.cpp

```cpp
#include "script.hpp"
#include <libriscv/guest_datatypes.hpp>

int main() {
    Script script("myscript", "guest.elf");

    // Call guest function with integers
    Event<int(int, int)> compute(script, "compute");
    if (auto ret = compute(17, 25))
        printf("Result: %d\n", *ret);

    // Pass string (guest receives const char*)
    Event<void(std::string)> greet(script, "greet");
    greet("World");

    // Pass std::string& via ScopedArenaObject (guest receives const std::string&)
    using ScopedStr = riscv::ScopedArenaObject<8, riscv::GuestStdString<8>>;
    ScopedStr str(script.machine(), "Arena World");
    Event<void(ScopedStr&)> greet_str(script, "greet_str");
    greet_str(str);
}
```

### Guest CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.14)
project(guest LANGUAGES CXX)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=gnu++20 -fno-threadsafe-statics -fno-stack-protector -O2")

function(add_guest_binary NAME)
    add_executable(${NAME} ${ARGN} env.cpp)
    target_link_libraries(${NAME} -static)
    target_link_libraries(${NAME} "-Wl,-Ttext-segment=0x400000")
    target_link_libraries(${NAME} "-Wl,--undefined=fast_exit")
    target_link_libraries(${NAME} "-Wl,--wrap=memcpy,--wrap=memset,--wrap=memcmp,--wrap=memmove")
    target_link_libraries(${NAME} "-Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc")
    target_link_libraries(${NAME} "-Wl,--wrap=strlen,--wrap=strcmp,--wrap=strncmp")
endfunction()

add_guest_binary(guest program.cpp)
```

Cross-compile with: `cmake -DCMAKE_TOOLCHAIN_FILE=toolchain.cmake -DCMAKE_BUILD_TYPE=Release ..`

Where `toolchain.cmake` contains:
```cmake
SET(CMAKE_SYSTEM_NAME Linux)
SET(CMAKE_CROSSCOMPILING 1)
set(CMAKE_TRY_COMPILE_TARGET_TYPE "STATIC_LIBRARY")
set(GCC_TRIPLE "riscv64-unknown-elf")
set(CMAKE_CXX_COMPILER "${GCC_TRIPLE}-g++")
```

The guest's `env.cpp` must provide `__wrap_malloc`, `__wrap_free`, `__wrap_memcpy`, etc. as ecall stubs. See the Override (Guest Side) section above, or copy `examples/gamedev/cpp_program/env.cpp` directly.

The guest's `program.cpp` may call `fast_exit(0)` instead of returning from `main()`, avoiding global destructors getting called (which closes stdout and clears many containers). And should expose callable (unreferenced) functions with `extern "C" __attribute__((used, retain))`. Referenced functions (eg. callbacks passed to host functions) will not get culled by the linker, and doesn't need this treatment.

### Verification

After building the guest ELF, verify that heap and memory operations are syscall stubs:

```
$ riscv64-unknown-elf-objdump -d guest.elf | grep -A4 '__wrap_malloc'
0000000000400c90 <__wrap_malloc>:
  400c90:   1ea00893   li   a7,490
  400c94:   00000073   ecall
  400c98:   00008067   ret

$ riscv64-unknown-elf-objdump -d guest.elf | grep -A4 '__wrap_memcpy'
0000000000401a84 <__wrap_memcpy>:
  401a84:   1ef00893   li   a7,495
  401a88:   00000073   ecall
  401a8c:   00008067   ret
```

If either shows a large function body instead of `li/ecall/ret`, the override failed — check your `--wrap` linker flags and that `env.cpp` is compiled into the binary.

---

## Verifying VMCall Latency

libriscv is designed for ultra-low-latency scripting. A properly configured setup achieves single-digit to low-teens nanosecond per vmcall overhead — the lowest of any known interpreter. This section shows how to measure it and explains the mechanism that makes it possible.

### Benchmark Guest Function

Add an empty function to the guest program. It does no work — the measured time is pure vmcall overhead:

```cpp
PUBLIC(void empty_function())
{
}
```

### Benchmark Host Code

Use `clock_gettime(CLOCK_MONOTONIC)` around batches of 10,000 vmcalls. Run 100 rounds and take the median to eliminate noise:

```cpp
#include <algorithm>
#include <time.h>
#include <vector>

static constexpr int CALLS_PER_ROUND = 10'000;
static constexpr int ROUNDS = 100;

Event<void()> bench(script, "empty_function");

std::vector<double> round_times_ns(ROUNDS);

for (int r = 0; r < ROUNDS; r++) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < CALLS_PER_ROUND; i++)
        bench();

    clock_gettime(CLOCK_MONOTONIC, &t1);

    double elapsed_ns = (t1.tv_sec - t0.tv_sec) * 1e9
        + (t1.tv_nsec - t0.tv_nsec);
    round_times_ns[r] = elapsed_ns / CALLS_PER_ROUND;
}

std::sort(round_times_ns.begin(), round_times_ns.end());
double median_ns = round_times_ns[ROUNDS / 2];
printf("Per-call latency: median=%.1f ns\n", median_ns);
```

Expected output on modern hardware:

| Mode | Median | Notes |
|---|---|---|
| Interpreted (`translate_enabled = false`) | ~5.7 ns | Lowest call overhead |
| Binary translation (JIT) | ~13.5 ns | JIT entry/exit overhead dominates for trivial calls |

Binary translation improves throughput for compute-heavy guest functions but adds overhead to the vmcall entry/exit path. If your hot path is many short calls (event dispatching, property getters), keep `translate_enabled = false`. Enable JIT when guest functions do significant work.

### The Exit Function: Dual Purpose

The `default_exit_function` option is the single most important setting for vmcall latency. It serves two purposes:

**Purpose 1: Pausing main() at boot.** The guest calls `fast_exit(0)` at the end of `main()`. The WFI instruction inside it stops the emulator, leaving the machine in a clean state with all globals initialized and no destructors run.

**Purpose 2: Instant vmcall return.** Every vmcall sets the return address register (RA) to the exit function's address. When the guest function executes `ret`, the PC lands on the exit function. Because the exit function lives within the guest's own execute segment, the emulator stays on the same decoded instruction stream. The WFI instruction is decoded as the `STOP` bytecode, which immediately ends simulation with no dispatch overhead.

Without `default_exit_function`, the emulator installs a fallback exit page at a separate address. Returning to it requires an execute segment switch — the slow path.

**Live-patching optimization:** On the first vmcall, when the emulator sees `ret` (JALR to RA) and RA equals the exit address, it hot-patches that instruction's bytecode from JALR to STOP. All subsequent calls to the same guest function return via STOP directly, skipping the RA comparison entirely.

This is why the guest must provide `fast_exit`:

```cpp
// In the guest's env.cpp
extern "C" __attribute__((noreturn, used, retain))
void fast_exit(int code)
{
    register long a0 asm("a0") = code;
    asm volatile("r%=: wfi \nj r%=\n" :: "r"(a0));
    __builtin_unreachable();
}
```

And the host must reference it:

```cpp
riscv::MachineOptions<8> options {
    // ...
    .default_exit_function = "fast_exit",
};
```

The linker flag `--undefined=fast_exit` in the guest CMakeLists.txt prevents the linker from stripping the function even if nothing in the guest calls it directly (only the host sets RA to its address).

---

## Summary of Integration Requirements

For full integration and best results:

1. **Use the flat arena** (default). Without it, you copy memory instead of viewing it.
2. **Take over the guest heap**. Without it, you cannot construct or read C++ types.
3. **Override malloc/free/memcpy in the guest**. Verify with objdump.
4. **Use PreparedCall for repeated calls**. Symbol lookup on every call is wasteful.
5. **Use sysargs with typed pointers** in host functions. `sysargs<MyStruct*>()` gives you a direct pointer.
6. **Use GuestStdString/GuestStdVector** for complex types. They mirror the exact libstdc++ memory layout.
7. **Don't let main() return**. Pause it with a meaningful host function to avoid global destructors getting called immediately after main() returns.
8. **Track call depth**. Use vmcall at depth 1, preempt at depth 2+, reject at max depth.
9. **Avoid C-like user-facing APIs**. Create proper classes and wrappers in the guest that lets users write normal code.
10. **Disable unused features for low memory**. With `RISCV_VIRTUAL_PAGING=OFF`, `RISCV_BINARY_TRANSLATION=OFF`, and only the needed architecture enabled (`RISCV_64I=ON`, `RISCV_32I=OFF`), a full integration with two VM instances runs at ~8.6 MB RSS.

### Reference Implementation

The [`examples/expert`](examples/expert) project is a complete, buildable implementation of every pattern described in this document. It exercises generated host functions, two-phase init, guest datatypes, RPC between VMs, vmcall latency benchmarking, and RSS measurement. Run `./build.sh` under `examples/expert` to build and execute the full test suite.

---

## RPC Between Same-Program Instances

When two guest VMs run the exact same binary, their code segments are identical: every function lives at the same address, every template instantiation produces the same trampoline. This makes it possible to extract a C++ lambda's function pointer and capture storage from one VM, transfer the capture bytes to another VM, and call the same function there. The result is as-if the lambda executed in the original VM, except it runs on a different instance's state.

### The Principle

A C++ lambda with captures has two parts:
1. A **function body** — compiled code at a fixed address in the binary
2. **Capture storage** — a small struct holding the captured values, living on the stack

A stateless lambda (no captures) converts to a plain function pointer via the unary `+` operator. By wrapping the captured lambda in a stateless trampoline that receives the capture storage through a `void*`, you separate the two parts:

```cpp
template <typename F>
void invoke_remotely(F callback) {
    sys_invoke_elsewhere(
        +[](void* data) { (*(F*)data)(); },  // trampoline: same address in every VM
        &callback,                             // capture storage: stack bytes
        sizeof(callback));                     // capture size
}
```

The trampoline is a template instantiation — its address is baked into the binary at compile time. Since both VMs loaded the same ELF, the address is valid in both. The capture bytes are plain data that can be copied between VMs without interpretation.

### Constraints on Capture Storage

The capture must be **self-contained binary data**:

| Safe to capture | Unsafe to capture |
|---|---|
| Integers, floats, enums | Pointers to stack variables |
| Small POD structs | `std::string` (contains a heap pointer) |
| Fixed-size arrays | `std::vector` (contains a heap pointer) |
| `constexpr` values | Non-const static references |

The rule: if the bytes are meaningful without access to the original VM's heap or stack, they're safe to capture. If they contain pointers, those pointers are meaningless in the other VM. The `riscv::Function` class in `lib/libriscv/util/function.hpp` enforces these constraints at compile time — it requires callables to be trivially copyable, trivially destructible, and fit within `FunctionStorageSize` (24 bytes).

### CaptureStorage Helper (Host Side)

The host needs to copy capture bytes out of a guest VM and later copy them into another. A simple fixed-size buffer is enough:

```cpp
struct CaptureStorage
{
    static constexpr size_t MaxSize = 32;
    using Array = std::array<uint8_t, MaxSize>;

    static Array get(machine_t& machine, gaddr_t data, gaddr_t size)
    {
        Array capture{};
        if (size > MaxSize)
            throw std::runtime_error("Capture storage exceeds 32 bytes");
        machine.memory.memcpy_out(capture.data(), data, size);
        return capture;
    }
};
```

### Guest-Side Wrappers

The guest provides two template wrappers. Both use the same trampoline pattern — only the generated host function differs. The RPC functions are defined in `host_functions.json` alongside all other host functions:

```json
{
  "typedef": [
    "typedef void (*rpc_callback_t)(void*)"
  ],
  "RPC::callback": "void sys_rpc_callback (rpc_callback_t, void*, size_t)",
  "RPC::invoke": "long sys_rpc_invoke (rpc_callback_t, void*, size_t)"
}
```

The guest calls `sys_rpc_callback` and `sys_rpc_invoke` like any other generated host function — no raw syscalls needed:

```cpp
// Store a lambda's capture and call it back on the same VM (local round-trip).
template <typename F>
static void store_and_callback(F callback) {
    static_assert(sizeof(F) <= 24, "Capture too large for storage");
    static_assert(std::is_trivially_copyable_v<F>, "Capture must be trivially copyable");
    sys_rpc_callback(
        +[](void* data) { (*(F*)data)(); },
        (void*)&callback, sizeof(callback));
}

// Invoke a lambda on a different VM running the same binary (RPC).
template <typename F>
static long invoke_elsewhere(F callback) {
    static_assert(sizeof(F) <= 24, "Capture too large for storage");
    static_assert(std::is_trivially_copyable_v<F>, "Capture must be trivially copyable");
    return sys_rpc_invoke(
        +[](void* data) { (*(F*)data)(); },
        (void*)&callback, sizeof(callback));
}
```

The `+[]` converts the stateless lambda to a function pointer. The `static_assert` guards catch misuse at compile time.

### Host-Side Function Handlers

These are registered through the generated host function system (see [Generated Host Functions](#generated-host-functions)), not as raw syscalls.

**RPC::callback — Local round-trip** stores the capture, then passes it directly as a vmcall argument. The vmcall pushes the capture bytes onto the guest stack and passes a pointer — no heap allocation needed:

```cpp
Script::set_host_function(
    "RPC::callback",
    "void sys_rpc_callback (rpc_callback_t, void*, size_t)",
    [](Script& script) {
        auto [func, data, size] =
            script.machine().sysargs<gaddr_t, gaddr_t, gaddr_t>();

        auto capture = CaptureStorage::get(script.machine(), data, size);

        script.call(func, capture);
    });
```

**RPC::invoke — Cross-VM call** does the same thing, but on a peer VM:

```cpp
Script::set_host_function(
    "RPC::invoke",
    "long sys_rpc_invoke (rpc_callback_t, void*, size_t)",
    [](Script& script) {
        auto [func, data, size] =
            script.machine().sysargs<gaddr_t, gaddr_t, gaddr_t>();

        if (!script.m_peer)
            throw std::runtime_error("No peer script configured for RPC");

        auto capture = CaptureStorage::get(script.machine(), data, size);

        auto& peer = *script.m_peer;
        auto result = peer.call(func, capture);

        script.machine().set_result(result.value_or(0));
    });
```

The key insight is that `capture` — a `std::array<uint8_t, 32>` — is passed directly as an argument to `call()`, which forwards it to `vmcall`. The vmcall pushes the array onto the guest stack and passes a pointer in the argument register. The capture bytes have call lifetime on the stack, which is exactly right since the trampoline only needs them for the duration of the call. No heap allocation, no free, no cleanup.

`func` is a guest address that exists at the same location in both VMs because they loaded the same binary. The capture bytes were copied from the source VM's registers/stack into the peer VM's call stack, making them accessible when the trampoline dereferences the `void*`.

### Wiring Up Peers

The host connects two Script instances before RPC calls can happen:

```cpp
Script script_a("script_a", "guest.elf");
Script script_b("script_b", "guest.elf");  // same binary

script_a.set_peer(&script_b);
script_b.set_peer(&script_a);
```

The `set_peer` method simply stores a pointer. In production, this could be a lookup table, a region-based routing system, or any mechanism that resolves "where should this RPC go."

### Complete Example

**Guest side** — a function that modifies a static counter via RPC:

```cpp
static int shared_counter = 0;

PUBLIC(int test_rpc_invoke())
{
    int delta = 10;
    invoke_elsewhere([delta]() {
        shared_counter += delta;
        printf("  Guest RPC target: shared_counter += %d, now = %d\n",
            delta, shared_counter);
    });
    return 0;
}

PUBLIC(int get_shared_counter())
{
    return shared_counter;
}
```

**Host side** — verifying that only the peer's state changed:

```cpp
Event<int()> get_counter_a(script_a, "get_shared_counter");
Event<int()> get_counter_b(script_b, "get_shared_counter");

printf("Before: a=%d, b=%d\n", *get_counter_a(), *get_counter_b());
// Before: a=0, b=0

Event<int()> rpc_test(script_a, "test_rpc_invoke");
rpc_test();  // script_a's lambda executes on script_b

printf("After: a=%d, b=%d\n", *get_counter_a(), *get_counter_b());
// After: a=0, b=10
```

The lambda captured `delta = 10` by value. Those 4 bytes were copied from script_a and pushed onto script_b's call stack via vmcall. The trampoline at the same code address in script_b dereferenced them and incremented script_b's `shared_counter`. Script_a's counter was never touched.

### Why This Works

1. **Same binary** → identical code layout. Function pointers are portable between instances.
2. **Trivially copyable captures** → the bytes are self-contained. No pointers to dereference, no destructors to run.
3. **Stateless trampoline** → the `+[]` lambda compiles to a plain function in `.text`. Its address is a link-time constant.
4. **Stack-pushed capture** → vmcall pushes the capture bytes onto the guest stack with call lifetime. The trampoline accesses them via the `void*` argument — no heap allocation needed.

### Extending the Pattern

The example uses `void()` lambdas, but the pattern generalizes to any signature. Pack extra arguments into the capture envelope:

```cpp
template <typename F>
static long invoke_elsewhere_with(F callback, double arg1, int arg2) {
    struct Envelope { F func; double a1; int a2; };
    static_assert(sizeof(Envelope) <= 24);
    Envelope env{callback, arg1, arg2};
    return sys_rpc_invoke(
        +[](void* data) {
            auto& e = *(Envelope*)data;
            e.func(e.a1, e.a2);
        },
        (void*)&env, sizeof(env));
}
```
