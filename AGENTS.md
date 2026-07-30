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
10. [Zero-Copy Structures](#zero-copy-structures)
11. [Production Integration](#production-integration)
12. [Verifying VMCall Latency](#verifying-vmcall-latency)
13. [Summary of Integration Requirements](#summary-of-integration-requirements)
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

The guest must route `malloc`, `free`, `memcpy` and friends to those syscalls. With newlib (`riscv64-unknown-elf-g++`) that takes two pieces. First, `--wrap` linker flags, which resolve `malloc` → `__wrap_malloc` (the original becomes `__real_malloc`):

```cmake
target_link_libraries(${NAME} "-Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc")
target_link_libraries(${NAME} "-Wl,--wrap=memcpy,--wrap=memset,--wrap=memcmp,--wrap=memmove")
target_link_libraries(${NAME} "-Wl,--wrap=strlen,--wrap=strcmp,--wrap=strncmp")
```

Second, the stubs themselves in a guest source file (`env.cpp`). malloc/free/calloc/realloc are pure asm:

```cpp
#define GENERATE_SYSCALL_WRAPPER(name, number) \
    asm(".global " #name "\n" #name ":\n  li a7, " STRINGIFY(number) "\n  ecall\n  ret\n");

asm(".pushsection .text, \"ax\", @progbits\n");
GENERATE_SYSCALL_WRAPPER(__wrap_malloc,  HEAP_SYSCALLS_BASE+0);
GENERATE_SYSCALL_WRAPPER(__wrap_free,    HEAP_SYSCALLS_BASE+3);
// ... similarly for calloc, realloc
asm(".popsection\n");
```

memcpy/memset need inline asm with real memory constraints, so the compiler knows what they touch:

```cpp
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

Copy [`examples/gamedev/cpp_program/env.cpp`](examples/gamedev/cpp_program/env.cpp) for the complete set. A Rust guest does not need any of this — see [Heap takeover in a Rust guest](#heap-takeover-in-a-rust-guest).

**Verification**: `objdump -d program.elf | grep -A4 '__wrap_malloc'` must show a 3-instruction stub (`li a7, N; ecall; ret`). A large function body means the takeover failed.

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

With heap takeover, the host can construct and pass standard library types that the guest understands, both C++ and Rust.

`<libriscv/guest_datatypes.hpp>` includes all of them. Each container also has its own header under `libriscv/guest/`, so you can include only what you use:

| Header | Contents |
| --- | --- |
| `guest/guest_common.hpp` | The traits and dispatch every container shares |
| `guest/guest_arena_object.hpp` | `ScopedArenaObject`: an owned object in the arena |
| `guest/guest_cpp_string.hpp` | `GuestStdString` |
| `guest/guest_cpp_vector.hpp` | `GuestStdVector` |
| `guest/guest_cpp_hash.hpp` | `GuestStdHash`, the replica of the guests `std::hash` |
| `guest/guest_cpp_unordered_map.hpp` | `GuestStdUnorderedMap` |
| `guest/guest_cpp_variant.hpp` | `GuestStdVariant` |
| `guest/guest_rust_string.hpp` | `GuestRustString`, `GuestRustStr` |
| `guest/guest_rust_vec.hpp` | `GuestRustVec`, `GuestRustSlice` |
| `guest/guest_rust_box.hpp` | `GuestRustBox`, `GuestRustBoxedSlice`, `GuestRustBoxedStr` |
| `guest/guest_rust_enum.hpp` | `GuestRustEnum`, the `#[repr(C, uN)]` enum |
| `guest/guest_rust_attributes.hpp` | `GuestRustAttr`, `GuestRustAttributes` |

A container is registered with the shared machinery by specializing three traits in `guest_common.hpp`: `is_guest_datatype` (it owns guest memory, `free()` releases it), `is_self_referencing_guest_object` (it points back into itself, so it needs `move()`), and `guest_object_needs_self_address` (its constructors take `(machine, self, ...)`). Everything else — vectors of it, variant alternatives, map keys and values, `ScopedArenaObject`, and passing it to `vmcall()` — then works without further changes.

[`examples/attribute_bench`](examples/attribute_bench) is a complete, buildable program that uses `GuestStdUnorderedMap`, `GuestStdVariant` and `GuestStdString` to move a tree of named attributes across the boundary in both directions, benchmarked against a hand-serialized flat form and checked for leaks. `tests/unit/native.cpp` and `tests/unit/native_rust.cpp` exercise every container against a real guest.

### Type Aliases (typical project setup)

```cpp
using CppString = riscv::GuestStdString<8>;                  // Mirrors libstdc++ std::string
template <typename T>
using CppVector = riscv::GuestStdVector<8, T>;               // Mirrors std::vector<T>
using CppStringVector = CppVector<CppString>;                // std::vector<std::string>
template <typename K, typename V>
using CppMap = riscv::GuestStdUnorderedMap<8, K, V>;         // Mirrors std::unordered_map<K, V>
template <typename... Types>
using CppVariant = riscv::GuestStdVariant<8, Types...>;      // Mirrors std::variant<Types...>

using RustString = riscv::GuestRustString<8>;                // Mirrors Rust String
template <typename T>
using RustVec = riscv::GuestRustVec<8, T>;                   // Mirrors Rust Vec<T>
template <typename T>
using RustBox = riscv::GuestRustBox<8, T>;                   // Mirrors Rust Box<T>
template <typename... Types>
using RustEnum = riscv::GuestRustEnum<8, uint64_t, Types...>; // #[repr(C, u64)] enum
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

### GuestStdUnorderedMap

Mirrors libstdc++'s `std::unordered_map<K, V>` layout: a bucket array plus a singly-linked list of every element. The hash function (`std::hash`), the bucket indexing and the bucket growth policy are all replicated exactly, so the guest can look up, insert and erase elements in a map the host created, and vice versa.

Because a bucket points at the element *before* the first element in that bucket, a non-empty map contains a pointer back into itself. Two consequences:

1. The map must live in guest memory (use `ScopedArenaObject`, or a pointer obtained from a system call argument). It cannot be built on the host stack.
2. Operations that can allocate take the address of the map itself as the `self` argument, and `move()` must be called when a map is relocated (`ScopedArenaObject` and `GuestStdVector` do this for you).

Supported key types are integers, enums, floats and `GuestStdString`. Specialize `riscv::GuestStdHash<W, K>` for other keys.

**A map with a custom hash function needs `CacheHashCode = false`.** libstdc++ stores the hash code of every key inside its node only when the hash function is one it marks as slow — `std::hash<std::string>` is, and a hash function written by the guest never is (`__is_fast_hash` defaults to true, even for one that just forwards to `std::hash`). The fourth template argument defaults to what `std::hash<K>` would give, so a map with its own hasher has to say otherwise, or every key and value in a node is read at the wrong offset:

```cpp
// guest: std::unordered_map<std::string, int, string_hash, std::equal_to<>>
using CppFastHashMap = riscv::GuestStdUnorderedMap<8, CppString, int, false>;
```

The hash *values* are unaffected: a transparent hasher that forwards to `std::hash<std::string_view>` produces exactly what `GuestStdHash` computes, so only the node layout differs.

```cpp
// Create a map for the guest, and fill it in
riscv::ScopedGuestStdUnorderedMap<8, CppString, int> map(machine);
map->insert_or_assign(machine, map.address(), "one", 1);
map->insert_or_assign(machine, map.address(), "two", 2);
map->erase(machine, map.address(), "one");
map->reserve(machine, map.address(), 100);

// Or assign a whole host map at once
map = std::unordered_map<std::string, int>{ {"one", 1}, {"two", 2} };

// The guest sees a real std::unordered_map<std::string, int>&
machine.vmcall<MAX>("function", map);

// Read a map from guest memory
int* value = map->find(machine, "two");        // Pointer into guest memory, or null
int& v     = map->at(machine, "two");          // Throws std::out_of_range
bool has   = map->contains(machine, "two");
map->for_each(machine, [&] (const CppString& key, int& value) {
    printf("%s = %d\n", key.to_string(machine).c_str(), value);
});
auto host_map = map->to_map(machine);          // Copy to a std::unordered_map

// Free (recursively frees contained strings, vectors and maps)
map->free(machine);
```

In a system call handler the address of the guests own map is needed as `self`:

```cpp
machine.install_syscall_handler(1, [] (auto& machine) {
    const auto self = machine.template sysarg<riscv::address_type<8>>(0);
    auto& map = *machine.memory.template memarray<CppMap<CppString, CppString>>(self, 1);
    map.insert_or_assign(machine, self, "key", "value");
    machine.set_result(0);
});
```

See [`examples/attribute_bench`](examples/attribute_bench) for this pattern in full, including the custom-hasher case and recursive freeing of a nested tree.

### GuestStdVariant

Mirrors `std::variant<Types...>` layout: the union of every alternative, followed by the index of the active alternative. Host-side alternatives are the mirror types, so a guest `std::variant<bool, int, double, glm::vec3, std::string>` becomes `riscv::GuestStdVariant<8, bool, int, double, glm::vec3, CppString>`.

Assigning a host value selects the alternative the same way the converting constructor of `std::variant` does: an exact match when there is one, a string alternative for strings, and otherwise the single alternative that the value converts to without narrowing. When no unique alternative matches, it is a compile error — use `emplace<T>()` to select one explicitly.

Like the map, a variant that holds a self-referencing alternative (a `std::string` with SSO, or a nested map) points back into itself, so operations that can allocate take the address of the variant itself as the `self` argument. The alternative is stored at the very beginning of the variant, so it is the same address.

```cpp
template <typename... Types>
using CppVariant = riscv::GuestStdVariant<8, Types...>;
using CppValue = CppVariant<bool, int, double, glm::vec3, CppString>;

riscv::ScopedGuestStdVariant<8, bool, int, double, glm::vec3, CppString> value(machine);
value = true;                       // Selects the bool alternative
value = 42;                         // int
value = 0.5;                        // double
value = glm::vec3(1, 2, 3);         // glm::vec3
value = "a string value";           // CppString
value->emplace<double>(machine, value.address(), 3.14);  // Explicit alternative
value->set(machine, value.address(), host_variant);      // From a host std::variant

// Reading a variant in guest memory
if (value->holds_alternative<double>())
    printf("%f\n", value->get<double>());
if (int* i = value->get_if<int>())
    printf("%d\n", *i);
value->visit([&] (auto& alt) {
    using T = std::decay_t<decltype(alt)>;
    if constexpr (std::is_same_v<T, CppString>)
        printf("%s\n", alt.to_string(machine).c_str());
});
auto host_value = value->to_variant(machine);  // Copy to a std::variant

value->free(machine);   // Frees the active alternative, leaving it valueless
```

The intended use is a guest-side `std::unordered_map<std::string, std::variant<...>>`, which the host can create, fill in, read back and modify:

```cpp
riscv::ScopedGuestStdUnorderedMap<8, CppString, CppValue> map(machine);
const auto self = map.address();
map->insert_or_assign(machine, self, "flag", true);
map->insert_or_assign(machine, self, "count", 42);
map->insert_or_assign(machine, self, "position", glm::vec3(1, 2, 3));
map->insert_or_assign(machine, self, "name", "a long name that is not SSO");

// The guest sees a real std::unordered_map<std::string, std::variant<...>>&
machine.vmcall<MAX>("function", map);

printf("%d\n", map->at(machine, "count").get<int>());
map->for_each(machine, [&] (const CppString& key, CppValue& value) {
    value.visit([&] (auto& alt) { /* ... */ });
});
```

Nested guest containers are supported as alternatives (`CppVector<int>`, `CppMap<K, V>`, another variant), and `free()` releases them recursively. Reading those must be done with `visit()`, as `to_variant()` only handles scalars and strings.

### GuestRustString and GuestRustVec

Rust's `String` is a `Vec<u8>` of UTF-8 bytes, and both are three machine words laid out as `{ capacity, ptr, len }`. There is no small-string optimization and no terminating zero, and neither points back into itself, so they can be moved around freely — no `move()` call is ever needed.

```cpp
RustString str(machine);
str.set_string(machine, "Hello");
str.append(machine, ", Rust World!");     // Rust's push_str
std::string_view view = str.to_view(machine);
str.free(machine);                        // Or use ScopedGuestRustString<8>

RustVec<uint32_t> vec(machine);
for (uint32_t i = 0; i < 10; i++)
    vec.push_back(machine, i);
auto host_vec = vec.to_vector(machine);
vec.free(machine);

// A Vec<String>
ScopedGuestRustVec<8, RustString> names(machine,
    std::vector<std::string>{ "one", "two", "three" });
```

`reserve()` grows the allocation exactly the way Rust's `RawVec` does, so a collection built here and one built by the guest end up with the same capacity. An empty collection holds a dangling (never null) pointer equal to `alignof(T)`, matching Rust's `NonNull<T>`.

`GuestRustStr` and `GuestRustSlice<T>` are the non-owning `&str` / `&[T]` fat pointers, `{ ptr, len }`. Note that this is the layout of a slice *stored in memory*; as a function argument the RISC-V ABI passes the two halves in separate registers, so a guest `fn(s: &str)` takes an address and a length as two integer arguments.

> **NOTE:** Rust does not stabilize the layout of its collections — the compiler reorders the fields, which is why the capacity comes first. The order above was verified by transmuting the collections to `[usize; N]` in a `riscv64gc-unknown-linux-gnu` program (rustc 1.93). Re-check it when a new Rust release lands. Only the slice layout is guaranteed by the language. `tests/unit/native_rust.cpp` asks the guest for those words on every run, so a future release that moves a field fails there.

### GuestRustBox

`Box<T>` is the only Rust smart pointer whose layout the language guarantees: for a sized `T` a single non-null pointer, ABI-compatible with `*mut T` and valid through `extern "C"`. The two unsized forms are the slice and `&str` fat pointers, with ownership.

```cpp
RustBox<int64_t> boxed(machine, int64_t(4321));   // Box::new(4321) on the guest heap
gaddr_t addr = boxed.release();                   // Box::into_raw - the guest owns it now
machine.vmcall<MAX>("take_box", addr);            // guest calls Box::from_raw and drops it

RustBox<int64_t> taken;
taken.assume_ownership(machine, guest_addr);      // Box::from_raw, host side
int64_t value = taken.get(machine);
taken.free(machine);                              // drops the value, then the block

GuestRustBoxedSlice<8, uint32_t> numbers(machine, std::vector<uint32_t>{1, 2, 3});  // Box<[u32]>
GuestRustBoxedStr<8> name(machine, "a boxed str");                                  // Box<str>
```

`Box::from_raw()` on a host allocation is *sound*, not merely convenient: the arena is the guest's global allocator, so the block really did come from the allocator that will release it. It is 16-byte aligned and does not need the size back, so Rust's `Layout` argument to `dealloc()` is ignored.

An empty or freed box is `ptr == 0`, ie. `Option<Box<T>>::None` — the one `Option` whose null representation the language documents. Do not extend that to `Option<String>` or `Option<Vec<T>>`: their niche is the *capacity* word (`None` is `0x8000000000000000` there), so a mirror testing the pointer would read `None` as an empty string. `Box<dyn Trait>` is `(data, vtable)` and is not mirrored.

### GuestRustEnum

The Rust answer to `std::variant`. The layout is not discovered but *requested*: `#[repr(C, uN)]` is the tag first, then the union of the variants. A plain `#[repr(Rust)]` enum cannot be mirrored — the compiler reorders the fields and hides the tag in a niche of one of them.

```rust
#[repr(C, u64)]          // u32 on a 32-bit guest
pub enum Value {
    Nil,                 // a unit variant is std::monostate on the host
    Int(i64),
    Float(f64),
    Str(String),
}
```

```cpp
using RustValue = riscv::GuestRustEnum<8, uint64_t,
    std::monostate, int64_t, double, RustString>;   // the order IS the ABI

RustValue value;
value.set(machine, std::string("hello"));           // picks the String variant
if (auto* s = value.get_if<RustString>()) { ... }
value.visit([&] (auto& alt) { ... });                // the general read
auto host = value.to_variant(machine);               // std::variant, scalars/strings only
value.free(machine);
```

The tag must be at least as wide as the alignment of the widest variant, which a `static_assert` enforces. RFC 2195 reads `repr(C, uN)` as a union of structs that each begin with the tag, putting a `bool` payload at offset 1 behind a `u8` tag; rustc emits a tag plus one uniformly aligned payload, putting it at offset 8. The two only differ when the tag is narrower than the payload alignment, so `#[repr(C, u64)]` removes the ambiguity for free — the padding after a `u8` tag would have been there anyway.

A Rust enum always holds one of its variants, so `free()` leaves the first variant in its default state. Declare a unit variant first and freeing twice is a no-op, not a double free.

Nothing takes a `self` address, so `emplace()` and `set()` are one argument shorter than the `GuestStdVariant` equivalents, and a `static_assert` rejects the C++ containers as variants.

### GuestRustAttributes

A bag of named, dynamically typed attributes — the Rust counterpart of the `unordered_map<string, variant>` tree in [`examples/attribute_bench`](examples/attribute_bench) — zero-copy in both directions:

```rust
#[repr(C)] pub struct Attr { pub key: String, pub value: Value }
#[repr(C)] pub struct Attributes { entries: Vec<Attr> }   // sorted by key
```

A sorted vector rather than a `HashMap`, on purpose. `std::HashMap` is hashbrown, whose table layout is `#[repr(Rust)]` and whose default hasher is seeded per process, so mirroring it would mean betting on both. A sorted `Vec` is ordinary Rust — `binary_search_by` for lookups — and faster here: one allocation for all the entries instead of a node per key, and no hasher to agree on. A guest wanting a real `HashMap` is one `.collect()` away. Keys compare as bytes, like Rust's `Ord for str`, so host and guest order agree. A tuple would have done for the entry, but its layout is unspecified and `#[repr(C)]` costs nothing.

```cpp
struct RustValue;                                        // must name itself
using RustAttrs = riscv::GuestRustAttributes<8, RustValue>;
struct RustValue : riscv::GuestRustEnum<8, uint64_t,
    std::monostate, int64_t, double, bool, RustString,
    riscv::GuestRustBox<8, RustAttrs>,                   // Group(Box<Attributes>)
    riscv::GuestRustBox<8, riscv::GuestRustVec<8, RustValue>>>   // List(Box<Vec<Value>>)
{
    using GuestRustEnum::GuestRustEnum;
};
RISCV_REGISTER_GUEST_RUST_ENUM(8, RustValue);            // one line, at namespace scope

RustAttrs attrs;
attrs.insert_or_assign(machine, "hp", int64_t(100));
attrs.insert_or_assign(machine, "name", std::string("player"));
auto& group = attrs.emplace<riscv::GuestRustBox<8, RustAttrs>>(machine, "stats").get(machine);
group.insert_or_assign(machine, "level", int64_t(7));

attrs.for_each(machine, [&] (std::string_view key, const RustValue& v) { ... });
int64_t hp = attrs.at(machine, "hp").get<int64_t>();
attrs.free(machine);        // frees the tree recursively, groups and lists included
```

A tree type must be a struct deriving from the enum, because the value type needs to name itself and only a class can be forward declared. `RISCV_REGISTER_GUEST_RUST_ENUM` registers it with the trait machinery and `static_assert`s that it adds no members.

`free()` walks the whole tree through the `free_guest_object` dispatch — the host-side counterpart of Rust's drop glue, and why there is no hand-written teardown like the C++ tree's `destroyGuestAttributes`. In the other direction the guest takes a host-built tree with one `Box::from_raw` and drops it normally.

Scripts are adversarial input, so `validate(machine, max_entries)` checks the shape before a walk and `is_sorted(machine)` reports keys out of order. Neither can corrupt host memory — the vector bounds every access, and a binary search that misses only fails to find.

#### Heap takeover in a Rust guest

A Rust guest takes over its heap with a `#[global_allocator]` that ecalls the native heap syscalls — no `--wrap` linker flags needed, because every `String` and `Vec` allocation goes through it:

```rust
struct SysAllocator;
unsafe impl GlobalAlloc for SysAllocator {
	unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
		let ret: *mut u8;
		asm!("ecall", in("a7") HEAP_SYSCALLS_BASE + 0,
			in("a0") layout.size(), in("a1") layout.align(), lateout("a0") ret);
		ret
	}
	// alloc_zeroed = +1 (calloc), realloc = +2, dealloc = +3
}

#[global_allocator]
static ALLOCATOR: SysAllocator = SysAllocator;
```

The arena always returns 16-byte aligned blocks, which covers every alignment the standard collections ask for. Build with `-C target-feature=+crt-static` so the ELF is statically linked, and keep exported `#[no_mangle]` functions from being collected with `-C link-args=-Wl,--no-gc-sections`. Rust's standard library locks with futexes even in a single-threaded program, so the host needs `machine.setup_posix_threads()` in addition to `setup_linux_syscalls()`.

[`examples/rust_expert`](examples/rust_expert) is a complete, buildable Rust guest with all of this wired up: the shared allocator, generated host functions emitted as Rust, `String` and `Vec` moving in both directions, closures called back through the host, and RPC between two VMs. It shares `host_functions.json` and the entire host with [`examples/expert`](examples/expert). Run its `build.sh`.

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
| `GuestRustString<W>` | Pushes the three words to the guest stack | `&String` / `*mut String` |
| `ScopedArenaObject<W, GuestRustString<W>>&` | Passes the guest heap address directly | `&String` / `*mut String` |

Any container registered as a guest datatype follows the same two rules: by value it is pushed to the stack (and told where it landed, so a self-referencing one stays valid), and in a `ScopedArenaObject` its arena address is passed. A Rust `&str` or `&[T]` argument is the exception — the ABI splits it into an address and a length in two registers, so pass `str.data(), str.size()`.

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

[`examples/expert/generate.py`](examples/expert/generate.py) is a complete, working generator — copy it as-is. It reads the JSON and writes two files that get compiled into the guest binary: a header with `extern` prototypes and typedefs, and a C source with the assembly stubs and the `dyncall_table`. The guest then calls the functions like any C API.

Each function becomes a 2-instruction stub — a custom RISC-V instruction encoding the table index, then `ret`:

```asm
.global sys_timer_periodic
sys_timer_periodic:
  .insn i 0b1011011, 0, x0, x0, 0    # table index 0
  ret
```

The emulator does not recognize opcode `0b1011011`, so it traps to a host callback. The index is in the instruction's 12-bit immediate (up to 2048 host functions), which is faster than ECALL: no register setup, no syscall dispatch table.

The `dyncall_table` in `.rodata` is what the host reads at init — a count followed by 16-byte entries of `{ crc32, reserved, strname, init_only, client_only, server_only, pad }`. `strname` is a *guest-space* address of a null-terminated name in `.rodata`, read with `machine().memory.memstring()`.

**CMake integration for the guest build:**

```cmake
set(JSON_FILE "${CMAKE_SOURCE_DIR}/host_functions.json")
set(PYPROGRAM "${CMAKE_SOURCE_DIR}/generate.py")
set(GEN_DIR   "${CMAKE_BINARY_DIR}/dyncalls")
set(GEN_FILES "${GEN_DIR}/host_functions")

file(MAKE_DIRECTORY ${GEN_DIR})
add_custom_command(
    OUTPUT ${GEN_FILES}.h ${GEN_FILES}.c
    COMMAND python3 ${PYPROGRAM} --verbose -j ${JSON_FILE} -o ${GEN_FILES}
    DEPENDS ${PYPROGRAM} ${JSON_FILE}
)
add_custom_target(generate_dyncalls ALL DEPENDS ${GEN_FILES}.h ${GEN_FILES}.c)
set_source_files_properties(${GEN_FILES}.h ${GEN_FILES}.c PROPERTIES GENERATED TRUE)

add_guest_binary(guest program.cpp ${GEN_FILES}.c)
add_dependencies(guest generate_dyncalls)
target_include_directories(guest PRIVATE ${GEN_DIR})

# dyncall_table lives in rodata and would otherwise be garbage-collected
target_link_libraries(guest "-Wl,--undefined=dyncall_table")
```

### Step 3: Host-Side Registration

The host registers handlers by name and signature; the signature is CRC32-hashed (`#include <libriscv/util/crc32.hpp>`) to match the guest's table. **Whitespace normalization is critical** — the generator hashes `" ".join(s.split())`, so the host must collapse runs of spaces the same way or nothing matches.

```cpp
using ghandler_t = std::function<void(Script&)>;
struct HostFunction { std::string name, signature; ghandler_t func; };

// Static, shared across all Script instances: CRC32(signature) -> handler
static std::map<uint32_t, HostFunction> m_host_functions;

void Script::set_host_function(
    std::string name, std::string signature, ghandler_t handler)
{
    signature = single_spaced(std::move(signature));
    const uint32_t hash = riscv::crc32(signature.c_str());

    auto it = m_host_functions.find(hash);
    if (it != m_host_functions.end() && it->second.name != name)
        throw std::runtime_error("CRC32 hash collision: " + name + " vs " + it->second.name);

    m_host_functions[hash] = {std::move(name), std::move(signature), std::move(handler)};
}
```

Registration uses the same signature string as the JSON:

```cpp
Script::set_host_function(
    "Timer::periodic",
    "int sys_timer_periodic (float, float, timer_callback, void*, size_t)",
    [](Script& script) {
        auto [interval, duration, callback, data, size] =
            script.machine().sysargs<float, float, gaddr_t, gaddr_t, gaddr_t>();
        // ... create timer ...
        script.machine().set_result(timer_id);
    });
```

### Step 4: Resolution at Init

At load time the host reads `dyncall_table` from guest rodata and builds a flat dispatch array — once, or twice for two-phase init. The entry layout must match what `generate.py` emits:

```cpp
struct HostFunctionDesc {
    uint32_t hash;
    uint32_t reserved;
    uint32_t strname;       // Guest-space address of the name string in .rodata
    bool initialization_only, client_side_only, server_side_only, padding;
};
static_assert(sizeof(HostFunctionDesc) == 16);
```

```cpp
void Script::resolve_host_functions(bool initialization, bool client_side)
{
    m_g_host_function_table = machine().address_of("dyncall_table");
    if (m_g_host_function_table == 0x0)
        throw std::runtime_error(name() + ": dyncall_table not found in guest ELF");

    // The first 4 bytes are the entry count, then the entries, viewed
    // directly out of guest rodata (zero-copy, flat arena)
    const uint32_t count = machine().memory.read<uint32_t>(m_g_host_function_table);
    if (count > 2048)
        throw std::runtime_error(name() + ": dyncall_table has bogus entry count");
    auto entries = machine().memory.memspan<const HostFunctionDesc>(
        m_g_host_function_table + 4, count);

    m_host_function_array.clear();
    m_host_function_array.reserve(count);

    for (auto& entry : entries) {
        // A function that is out of phase, or on the wrong side, becomes a
        // stub that throws if the guest calls it anyway
        const bool allowed =
            !(entry.initialization_only && !initialization) &&
            !(entry.client_side_only && !client_side) &&
            !(entry.server_side_only && client_side);

        auto it = m_host_functions.find(entry.hash);
        if (allowed && it != m_host_functions.end()) {
            m_host_function_array.push_back(it->second.func);
        } else {
            // The guest wants a function this host does not provide
            auto func_name = machine().memory.memstring(entry.strname);
            fprintf(stderr, "WARNING: Unresolved host function '%s' (hash %08x)\n",
                func_name.c_str(), entry.hash);
            m_host_function_array.push_back([](auto&) {
                throw std::runtime_error("Unresolved host function");
            });
        }
    }
}
```

`memstring(addr)` reads a null-terminated string from guest memory — `strname` points at the name that `generate.py` placed in `.rodata` next to the stubs.

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

## Zero-Copy Structures

With the flat arena, a whole guest structure can be viewed in place. The guest defines it with normal C++ types, and the host defines a mirror struct with the same layout:

```cpp
// Guest                                  // Host mirror
struct Dialogue {                         struct GuestDialogue {
    std::string name;                         CppString name;
    std::string portrait;                     CppString portrait;
    std::vector<DialogueText> texts;          CppVector<GuestText> texts;
    bool cancellable;                         bool cancellable;
    std::vector<DialogueChoice> choices;      CppVector<GuestChoice> choices;
};                                        };

// Guest hands over the address
sys_npc_do_dialogue(entity_uid, &dialogue, sizeof(dialogue));
```

In the host function handler:

```cpp
auto [uid, g_view, g_size] = script.machine().sysargs<uint32_t, GuestDialogue*, gaddr_t>();
if (g_size != sizeof(GuestDialogue)) { /* layout mismatch */ }

auto& dialogue = *g_view;                                  // no copy
std::string_view name = dialogue.name.to_view(machine);    // no copy

for (auto& text : dialogue.texts.to_span(machine))         // no copy
    std::string content = text.text.to_string(machine);    // copies just this one
```

The `sizeof` check is the version guard: a field added on either side is caught at the boundary instead of misreading every field after it.

**This only works with the flat arena.** With virtual paging the structure may span page boundaries and the pointer would be invalid.

A guest function pointer found inside such a structure can be turned into a callable directly:

```cpp
if (dialogue.nextScriptFunction.address) {
    Event<void(int)> event(script, dialogue.nextScriptFunction.address);
    // Call it later to trigger the next dialogue step
}
```

---

## Production Integration

A production wrapper puts a `Script` class around `Machine<8>`: call-depth tracking, a cached symbol lookup, guest heap allocation, per-thread forks, host function resolution, and error handling with backtraces. [`examples/expert`](examples/expert) is a complete buildable version of everything below; run its `build.sh`.

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

    template <typename... Args>
    std::optional<sgaddr_t> call(const std::string& func, Args&&... args);
    template <typename... Args>
    std::optional<sgaddr_t> call(gaddr_t addr, Args&&... args);

    gaddr_t address_of(const std::string& name) const; // cached lookup
    auto& machine() { return *m_machine; }
    const auto& name() const noexcept { return m_name; }

    gaddr_t guest_alloc(gaddr_t bytes) { return machine().arena().malloc(bytes); }
    bool guest_free(gaddr_t addr) { return machine().arena().free(addr) == 0x0; }

private:
    std::unique_ptr<machine_t> m_machine;
    std::vector<uint8_t> m_binary;
    std::string m_name;
    gaddr_t m_heap_area = 0;
    uint8_t m_call_depth = 0;
    mutable std::unordered_map<std::string, gaddr_t> m_lookup_cache;
};

// vmcall at depth 1 (owns CPU state), preempt at depth 2+, reject at max
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

// Type-safe wrapper for a guest function, resolved once
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

`Event` can also select the current thread's fork instead of a fixed instance — each thread gets its own via `Script::get_fork()`, sharing the read-only binary and host function table but with independent registers, stack and heap. No locking during execution.

### Script Implementation (script.cpp)

```cpp
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

    machine().set_userdata<Script>(this);
    machine().set_printer([](const machine_t&, const char* p, size_t len) {
        printf("%.*s", (int)len, p);
    });
    m_heap_area = machine().memory.mmap_allocate(MAX_HEAP);
    machine().setup_linux_syscalls(false, false); // no fs, no sockets
    machine().setup_native_heap(HEAP_SYSCALLS_BASE, m_heap_area, MAX_HEAP);
    machine().setup_native_memory(MEMORY_SYSCALLS_BASE);

    machine().setup_linux({name}, {"LC_CTYPE=C"});
    machine().simulate(MAX_BOOT_INSTR);     // runs main() until fast_exit pauses it
}
```

### Boot Sequence

```
1. Construct Machine with ELF binary + options
2. Install syscall handlers, native heap/memory/threads
3. machine.simulate(MAX_BOOT_INSTR)     -- runs main() until it pauses
4. resolve_host_functions(init=true)    -- all host functions available
5. call(on_init_addr)                   -- guest registers blocks, items, NPCs
6. resolve_host_functions(init=false)   -- lock out init-only functions
7. Machine is ready for vmcalls
```

### Host main.cpp

```cpp
Script script("myscript", "guest.elf");

Event<int(int, int)> compute(script, "compute");
if (auto ret = compute(17, 25))
    printf("Result: %d\n", *ret);

// Guest receives const char*
Event<void(std::string)> greet(script, "greet");
greet("World");

// Guest receives const std::string&
using ScopedStr = riscv::ScopedArenaObject<8, riscv::GuestStdString<8>>;
ScopedStr str(script.machine(), "Arena World");
Event<void(ScopedStr&)> greet_str(script, "greet_str");
greet_str(str);
```

### Guest CMakeLists.txt

```cmake
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

Cross-compile with `-DCMAKE_TOOLCHAIN_FILE=toolchain.cmake`, where the toolchain file sets `CMAKE_SYSTEM_NAME Linux`, `CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY` and `CMAKE_CXX_COMPILER riscv64-unknown-elf-g++`.

The guest's `env.cpp` provides the `__wrap_*` ecall stubs and `fast_exit` — copy [`examples/gamedev/cpp_program/env.cpp`](examples/gamedev/cpp_program/env.cpp). `program.cpp` should call `fast_exit(0)` instead of returning from `main()`, and expose unreferenced callables with `extern "C" __attribute__((used, retain))`.

### Verification

```
$ riscv64-unknown-elf-objdump -d guest.elf | grep -A4 '__wrap_malloc'
0000000000400c90 <__wrap_malloc>:
  400c90:   1ea00893   li   a7,490
  400c94:   00000073   ecall
  400c98:   00008067   ret
```

A large function body instead of `li/ecall/ret` means the override failed — check the `--wrap` flags and that `env.cpp` is in the binary.

---

## Verifying VMCall Latency

A properly configured setup reaches single-digit nanosecond vmcall overhead. To measure it, add an empty guest function and time batches of it:

```cpp
PUBLIC(void empty_function()) {}   // guest
```

```cpp
// host: 100 rounds of 10'000 calls with clock_gettime(CLOCK_MONOTONIC),
// dividing each round by CALLS_PER_ROUND and taking the median
Event<void()> bench(script, "empty_function");
```

| Mode | Median | Notes |
|---|---|---|
| Interpreted (`translate_enabled = false`) | ~5.7 ns | Lowest call overhead |
| Binary translation (JIT) | ~13.5 ns | JIT entry/exit dominates for trivial calls |

Binary translation improves throughput for compute-heavy guest functions but adds overhead to the vmcall entry/exit path. If the hot path is many short calls (event dispatch, property getters), keep `translate_enabled = false`.

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
2. **Take over the guest heap**. Without it, you cannot construct or read the guest's own container types.
3. **Override malloc/free/memcpy in the guest** (a `#[global_allocator]` in Rust). Verify with objdump.
4. **Use PreparedCall for repeated calls**. Symbol lookup on every call is wasteful.
5. **Use sysargs with typed pointers** in host functions. `sysargs<MyStruct*>()` gives you a direct pointer.
6. **Use the guest datatypes** for complex types. They mirror the exact libstdc++ and Rust memory layouts.
7. **Don't let main() return**. Pause it with a meaningful host function to avoid global destructors getting called immediately after main() returns.
8. **Track call depth**. Use vmcall at depth 1, preempt at depth 2+, reject at max depth.
9. **Avoid C-like user-facing APIs**. Create proper classes and wrappers in the guest that lets users write normal code.
10. **Disable unused features for low memory**. With `RISCV_VIRTUAL_PAGING=OFF`, `RISCV_BINARY_TRANSLATION=OFF`, and only the needed architecture enabled (`RISCV_64I=ON`, `RISCV_32I=OFF`), a full integration with two VM instances runs at ~8.6 MB RSS.

### Reference Implementation

[`examples/expert`](examples/expert) is a complete, buildable implementation of every pattern in this document: generated host functions, two-phase init, guest datatypes, RPC between VMs, vmcall latency benchmarking and RSS measurement. Run its `build.sh`.

Two more places worth reading:

| Where | What it shows |
| --- | --- |
| [`examples/attribute_bench`](examples/attribute_bench) | `GuestStdUnorderedMap` / `GuestStdVariant` / `GuestStdString` moving a tree of attributes both ways, benchmarked against a hand-serialized form, with leak accounting |
| `tests/unit/native.cpp`, `tests/unit/native_rust.cpp` | Every guest datatype exercised against a real C++ and Rust guest, including the layout probes |

---

## RPC Between Same-Program Instances

Two VMs running the exact same binary have identical code segments: every function, including every template instantiation, lives at the same address. That makes it possible to take a C++ lambda's function pointer and capture bytes from one VM and call it on another — as if the lambda ran in the first VM, but against the second one's state.

### The Principle

A captured lambda is a function body at a fixed address plus a small capture struct on the stack. A stateless lambda converts to a plain function pointer with unary `+`, so wrapping the captured one in a stateless trampoline that takes the capture through a `void*` separates the two:

```cpp
template <typename F>
static long invoke_elsewhere(F callback) {
    static_assert(sizeof(F) <= 24, "Capture too large for storage");
    static_assert(std::is_trivially_copyable_v<F>, "Capture must be trivially copyable");
    return sys_rpc_invoke(
        +[](void* data) { (*(F*)data)(); },  // trampoline: same address in every VM
        (void*)&callback, sizeof(callback)); // capture storage: stack bytes
}
```

The RPC entry points are ordinary generated host functions:

```json
{
  "typedef": ["typedef void (*rpc_callback_t)(void*)"],
  "RPC::callback": "void sys_rpc_callback (rpc_callback_t, void*, size_t)",
  "RPC::invoke": "long sys_rpc_invoke (rpc_callback_t, void*, size_t)"
}
```

### Constraints on Capture Storage

The capture must be self-contained binary data — meaningful without the original VM's heap or stack:

| Safe to capture | Unsafe to capture |
|---|---|
| Integers, floats, enums | Pointers to stack variables |
| Small POD structs | `std::string` (contains a heap pointer) |
| Fixed-size arrays | `std::vector` (contains a heap pointer) |
| `constexpr` values | Non-const static references |

`riscv::Function` in `lib/libriscv/util/function.hpp` enforces this at compile time: trivially copyable, trivially destructible, and at most `FunctionStorageSize` (24) bytes.

### Host-Side Handler

The host copies the capture bytes out of the calling VM and passes the array straight to `call()` on the peer. `vmcall` pushes it onto the peer's stack and hands the trampoline a pointer — call lifetime, no heap allocation, no cleanup:

```cpp
Script::set_host_function(
    "RPC::invoke",
    "long sys_rpc_invoke (rpc_callback_t, void*, size_t)",
    [](Script& script) {
        auto [func, data, size] =
            script.machine().sysargs<gaddr_t, gaddr_t, gaddr_t>();
        if (!script.m_peer)
            throw std::runtime_error("No peer script configured for RPC");

        std::array<uint8_t, 32> capture{};
        if (size > capture.size())
            throw std::runtime_error("Capture storage exceeds 32 bytes");
        script.machine().memory.memcpy_out(capture.data(), data, size);

        auto result = script.m_peer->call(func, capture);
        script.machine().set_result(result.value_or(0));
    });
```

`RPC::callback` is the same handler without the peer — a local round-trip through the host.

### End to End

```cpp
// Host: wire two instances of the same binary together
Script script_a("script_a", "guest.elf");
Script script_b("script_b", "guest.elf");
script_a.set_peer(&script_b);
script_b.set_peer(&script_a);
```

```cpp
// Guest
static int shared_counter = 0;

PUBLIC(int test_rpc_invoke()) {
    int delta = 10;
    invoke_elsewhere([delta]() { shared_counter += delta; });
    return 0;
}
PUBLIC(int get_shared_counter()) { return shared_counter; }
```

Calling `test_rpc_invoke` on `script_a` leaves `a=0, b=10`: the 4 captured bytes were copied from a's registers onto b's call stack, and the trampoline at the same code address incremented b's counter.

### Extending the Pattern

Extra arguments go into the capture envelope, so any signature works:

```cpp
template <typename F>
static long invoke_elsewhere_with(F callback, double arg1, int arg2) {
    struct Envelope { F func; double a1; int a2; };
    static_assert(sizeof(Envelope) <= 24);
    Envelope env{callback, arg1, arg2};
    return sys_rpc_invoke(
        +[](void* data) { auto& e = *(Envelope*)data; e.func(e.a1, e.a2); },
        (void*)&env, sizeof(env));
}
```
