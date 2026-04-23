# Expert example

A complete production-style integration of _libriscv_ demonstrating every major feature in a single self-contained project. The example builds both a RISC-V guest program and a host application, runs a comprehensive test suite, and reports vmcall latency and memory usage.

## What it covers

- Generated host functions via JSON definition, Python code generator, and CRC32-based resolution at init
- Two-phase initialization with init-only function lockout
- Custom instruction dispatch (opcode `0b1011011`)
- Type-safe `Event<F>` wrappers for calling guest functions
- Call depth tracking with vmcall at depth 1, preempt at depth 2+
- Passing strings as `const char*` and `const std::string&` via `ScopedArenaObject`
- Host-allocated vectors on the guest heap
- Guest ownership of host-allocated memory (move semantics across the boundary)
- Local lambda callbacks with capture storage
- RPC between two VMs running the same binary
- vmcall latency benchmark (10k calls x 100 rounds)
- RSS measurement after all tests complete

## Building and running

```
./build.sh
```

The script cross-compiles the guest with `riscv64-unknown-elf-g++`, builds the host with the system compiler, and runs the test suite. The host is built with paging disabled, binary translation off, and only RV64 enabled, resulting in ~8.6 MB RSS after running all tests including a second VM instance for the RPC test.

## Project structure

```
host_functions.json    Shared API definition (single source of truth)
generate.py            Produces guest stubs + dyncall_table from JSON
guest/
  program.cpp          Guest-side test functions and RPC support
  env.cpp              Heap/memory overrides and fast_exit
  CMakeLists.txt       Cross-compilation with --wrap linker flags
host/
  src/main.cpp         Test harness exercising every feature
  src/script.hpp       Script wrapper, Event system, call depth tracking
  src/script.cpp       Machine setup, host function resolution, dispatch
  src/functions.cpp    Host function implementations
  CMakeLists.txt       Host build linking against libriscv
```

## Low memory configuration

The build disables features not needed for interpreted-only embedding:

```
-DRISCV_VIRTUAL_PAGING=OFF     No page tables, flat arena only
-DRISCV_BINARY_TRANSLATION=OFF No JIT compilation
-DRISCV_32I=OFF                No 32-bit support compiled in
-DRISCV_64I=ON                 64-bit RISC-V only
```

Guest memory is capped at 16 MB with an 8 MB heap and 1 MB stack. The host process RSS after running the full test suite (including spawning a second VM for RPC) is ~8.6 MB.
