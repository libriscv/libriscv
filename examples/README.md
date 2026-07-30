# Examples

## SDL D00M

A sandboxed playable D00M example.

## Embed

An example that shows how to fetch libriscv from github and embed it in your CMake project.

## MSVC example

An example that works in Visual Studio, and allows you to run the [example binaries](/tests/unit/elf) from PowerShell.

## Noexcept example

An example that shows how to embed _libriscv_ in a project that disables C++ exceptions. _libriscv_ still requires them for itself, however it can be cordoned off, and all public headers are designed to support builds with disabled exceptions.

## Package example

An example that uses _libriscv_ when installed on the system as a package.

## WASM example

An example that builds a RISC-V programs with JIT-enabled LuaJIT inside, and then adds that to a WebAssembly program. The program is pre-built so that users can test immediately with `emrun`.

## Expert example

A complete production-style integration exercising every major feature: generated host functions, two-phase init, RPC between VMs, guest datatypes, and vmcall latency benchmarking. Built with paging disabled and binary translation off for ~8.6 MB RSS.

## Rust expert example

The expert example with the guest written in Rust, from the same `host_functions.json`. A Python generator translates the C signatures into a Rust library: `extern "C"` declarations, `global_asm!` stubs, the `dyncall_table`, and safe idiomatic wrappers grouped by namespace, so the program calls `io::print("hello")` and `data::fill_string(&mut text)`. The guest's `#[global_allocator]` forwards to the host arena, so a `String` or `Vec` built on either side is usable, growable and freeable on the other. Both generators hash the same signature strings, so a Rust guest and a C++ guest are interchangeable in front of one host.

## Attribute marshalling benchmark

Measures the cost of moving a tree of named attributes across the guest/host boundary two ways: serialized into a flat array of nodes, versus zero-copy, where the host walks the guest's own `std::unordered_map` in place (`GuestStdUnorderedMap`/`GuestStdVariant`/`GuestStdString`) and builds the guest's map directly when the traffic goes the other way. Both directions, three workload shapes, with a checksum and a guest-heap leak check on every path.

Run `./build.sh`. Zero-copy is ~2x the wall-clock in both directions, but sending a tree out drops from thousands of emulated guest instructions to seven, and receiving one halves the guest heap traffic.

## Attribute marshalling benchmark, Rust guest

The same benchmark with the guest written in Rust: a `#[repr(C, u64)]` enum and a `#[repr(C)]` sorted `Vec` of entries, mirrored on the host by `GuestRustAttributes`/`GuestRustEnum`/`GuestRustString`. The guest's `#[global_allocator]` is the host arena, so the host builds the tree in its final shape and the guest takes it with one `Box::from_raw`; the host mirror's `free()` is the counterpart of Rust's drop glue, so neither side needs a hand-written teardown. A layout probe compares the guest's own view of the ABI against the host mirror before anything is measured.

Run `./build.sh`. The numbers land within a few percent of the C++ guest's, which is the point: none of hashbrown's `#[repr(Rust)]` table layout or libstdc++'s hash-caching rule has to be mirrored to get there.

## WebAPI example

An example that uses a WebServer and Varnish Cache to implement a RISC-V playground.

## Per-request isolation example

An epoll HTTP server that creates a fresh Copy-on-Write fork of a master RISC-V VM for every incoming request. The guest handles routing and response generation entirely inside the sandboxed VM; the host only shuttles raw request bytes in and response bytes out through two custom syscalls. The fork is destroyed once the response is sent.

All guest string and memory operations are routed through host-accelerated native syscalls (`native_libc.h`) so no glibc code pages are touched in the hot path, keeping the fork+run+destroy cycle in the low single-digit microseconds. `RISCV_FLAT_RW_ARENA` must be off — flat arenas cannot be Copy-on-Write forked.

Run `./build_and_run.sh` to build and start the server, then `./benchmark.sh` to measure throughput.

### Example output

```
────────────────────────────────────────────
  Client-side (end-to-end, incl. TCP)
────────────────────────────────────────────
  Requests          2000
  Total time        205.9 ms
  Avg per request   103.0 µs
  Throughput        9712 req/s

────────────────────────────────────────────
  Server-side (fork + run + destroy)
────────────────────────────────────────────
  Samples           2001
  Avg total         3.4 µs
  Avg run-only      3.3 µs
  Min / p50 / p99   3 / 3 / 9 µs
  Max               41 µs
────────────────────────────────────────────
```
