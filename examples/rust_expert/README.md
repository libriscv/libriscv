# Rust expert example

The [expert example](../expert) with the guest rewritten in Rust. The host is unchanged: the same `host_functions.json`, the same CRC32-based resolution, the same `dyncall_table`, the same custom instruction. The generator emits a Rust library instead of C stubs, so the guest calls the host the way it calls any other crate.

## What it covers

- Generated host functions via JSON definition, Python code generator, and CRC32-based resolution at init, emitted as Rust `extern "C"` declarations, `global_asm!` stubs, `dyncall_table` and safe wrappers
- A `#[global_allocator]` forwarding to the host arena, so both sides allocate out of the same heap
- Two-phase initialization with init-only function lockout
- Custom instruction dispatch (opcode `0b1011011`)
- Type-safe `Event<F>` wrappers for calling guest functions
- Call depth tracking with vmcall at depth 1, preempt at depth 2+
- Passing `&str`, `&String` and `&Vec<i32>` from host to guest
- A `String` returned by value from the guest, allocated by it, freed by the host
- Host functions filling in a guest-owned `String` and `Vec<u32>`
- Guest ownership of host-allocated memory (move semantics across the boundary)
- Rust closures called back through the host, locally and into a second VM
- RPC between two VMs running the same binary
- vmcall latency benchmark (10k calls x 100 rounds)
- RSS measurement after all tests complete

## Building and running

```
./build.sh
```

Requirements:

- A Rust toolchain with the `riscv64gc-unknown-linux-gnu` standard library: `rustup target add riscv64gc-unknown-linux-gnu`
- A cross-linker, by default `riscv64-linux-gnu-gcc-12`, overridable with `CARGO_TARGET_RISCV64GC_UNKNOWN_LINUX_GNU_LINKER` or in `guest/.cargo/config.toml`
- `python3`, for the generator

The script generates the Rust host function module, cross-compiles the guest with cargo, builds the host with the system compiler, and runs the test suite. The suite passes in about a second with ~5 ns vmcall latency and ~17 MB RSS. The extra memory over the C++ example is a statically linked Rust standard library: a 6 MB ELF before it allocates anything.

## Project structure

```
host_functions.json    Shared API definition (single source of truth)
generate.py            Produces the Rust module from the JSON
guest/
  Cargo.toml           Workspace: the generated library, and the program
  .cargo/config.toml   Target, cross-linker, static linking, link args
  host_functions/
    build.rs           Runs generate.py into OUT_DIR
    src/lib.rs         include!()s the generated module
  program/
    src/main.rs        Guest-side test functions and RPC support
    src/env.rs         Global allocator over the host arena, fast_exit
host/
  src/main.cpp         Test harness exercising every feature
  src/script.hpp       Script wrapper, Event system, call depth tracking
  src/script.cpp       Machine setup, host function resolution, dispatch
  src/functions.cpp    Host function implementations
  CMakeLists.txt       Host build linking against libriscv
```

## The shared JSON definition

`host_functions.json` is a copy of the C++ example's: one C signature per host function.

```json
"Math::add":        "int sys_math_add (int a, int b)",
"IO::print":        "void sys_print (const char* text, size_t len)",
"Data::fill_string":"void sys_data_fill_string (rust_string_t* out)",
```

The signature string stays C because it *is* the identity of the function: the generator hashes it with CRC32 and the host hashes the same string when registering a handler. Both generators produce the same hash from the same declaration, so a Rust guest and a C++ guest are interchangeable in front of one host. Parameter names are optional, but naming them gives the Rust wrappers real argument names instead of `arg0`, `arg1`.

Two keys are new:

- `"rust": { "types": { ... } }` maps a C typedef to the Rust type it stands for. `rust_string_t` is `String`, `rpc_callback_t` is `extern "C" fn(*mut c_void)`. An unknown C type is an error, not a guess.
- `"rust": { "prelude": [ ... ] }` is emitted verbatim at the top of the generated module.

`examples/expert/generate.py` does not know about the `"rust"` key and would need it added to its reserved keys to read this file. Everything else is common to both.

## What the generator produces

Three things per entry. An `extern "C"` declaration, with C types translated to Rust:

```rust
pub mod raw {
    extern "C" {
        // IO::print: 0x499f282a
        pub fn sys_print(text: *const c_char, len: usize);
    }
}
```

The stub it resolves to, and the name string the host reads out of the table at load time. Same two-instruction stub as the C generator emits, written with `global_asm!` instead of a `.c` file:

```rust
global_asm!(r#"
.pushsection .text
.global sys_print
sys_print:
  .insn i 0b1011011, 0, x0, x0, 2
  ret
.popsection
"#);
```

And a safe wrapper, in a module named after the JSON namespace:

```rust
pub mod io {
    /// `void sys_print (const char* text, size_t len)`
    #[inline]
    pub fn print(text: &str) {
        unsafe { raw::sys_print(text.as_ptr() as *const c_char, text.len()) }
    }
}
```

Wrappers are built from the parameter list by four rules, applied in order:

| Parameters | Wrapper takes | Notes |
|---|---|---|
| `const char*` followed by `size_t` | `&str` | passed as pointer and length, no copy |
| `const char*` alone | `&str` | copied into a `CString` for the call |
| `T*` / `const T*`, `T` named in `"rust": {"types"}` | `&mut T` / `&T` | |
| anything else | unchanged | a leftover raw pointer makes the wrapper `unsafe fn` |

`RPC::invoke` takes a callback and a blob of captured bytes, so it stays an `unsafe fn` - correctly, since only the caller knows the two agree. Everything else ends up safe, and the program writes no `unsafe` outside its own trampolines.

The generated file lands in `guest/target/riscv64gc-unknown-linux-gnu/release/build/host_functions-*/out/host_functions.rs`.

## Rust values across the boundary

The guest's `#[global_allocator]` calls the host arena (`env.rs`, syscalls 490-493), so a collection allocated on either side is usable, growable and freeable on the other. Nothing is serialized and nothing is copied.

The host builds one and calls in:

```cpp
ScopedRustString str(script.machine(), "Arena World");
Event<void(ScopedRustString&)> greet_string(script, "greet_string");
greet_string(str);
```

```rust
#[no_mangle]
pub extern "C" fn greet_string(name: *const String) {
    let name: &String = unsafe { &*name };
    hprint!("Hello, {name}!");
}
```

The guest asks the host to fill one in, through a generated host function:

```rust
let mut text = String::new();
data::fill_string(&mut text);      // safe wrapper, &mut String
text.push_str(" ...and the guest appended to it");
```

```cpp
auto& str = *script.machine().memory.memarray<RustString>(self, 1);
str.set_string(script.machine(), "A String the host wrote into the guest");
```

The append reallocates the host's buffer through the guest's allocator in the same arena, and the guest drops it at the end of the function. The host-side halves, `GuestRustString` and `GuestRustVec`, are covered by `tests/unit/native_rust.cpp`, including a layout probe that asks the guest to confirm the field order the host mirrors assume.

## Notes on the guest build

Three settings in `guest/.cargo/config.toml` are load-bearing:

```
-C target-feature=+crt-static   Static ELF with no interpreter, and non-PIE,
                                which the absolute addresses in dyncall_table need
-Wl,--no-gc-sections            The #[no_mangle] functions the host calls have
                                no callers inside the guest
-Wl,--undefined=dyncall_table   Two symbols only ever referenced from the host
-Wl,--undefined=fast_exit
```

And in `Cargo.toml`, `strip = false`: the host looks guest functions up by name in `.symtab`.

## Low memory configuration

The same build options as the C++ example:

```
-DRISCV_VIRTUAL_PAGING=OFF     No page tables, flat arena only
-DRISCV_BINARY_TRANSLATION=OFF No JIT compilation
-DRISCV_32I=OFF                No 32-bit support compiled in
-DRISCV_64I=ON                 64-bit RISC-V only
```

Guest memory is capped at 64 MB with a 16 MB arena and a 2 MB stack. The host also calls `setup_posix_threads()`, which the C++ guest does not need: the Rust standard library locks with futexes even when single-threaded. RSS after the full test suite, including the second VM for RPC, is ~17 MB.
