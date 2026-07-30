# Attribute marshalling benchmark, Rust guest

The same measurement as [`examples/attribute_bench`](../attribute_bench), with the
guest written in Rust instead of C++. It costs what it costs to move a structured,
dynamically-typed object (a tree of named attributes) across the guest/host
boundary, comparing two strategies:

- **flat nodes**: the tree is serialized into an array of self-describing nodes,
  one per key, with strings and nested groups in separate allocations. The
  receiving side walks the array and rebuilds the tree.
- **zero-copy**: nothing is serialized. The guest hands over the address of its
  own `Attributes` and the host walks it in place with
  `riscv::GuestRustAttributes` / `GuestRustEnum` / `GuestRustString`; going the
  other way, the host builds the tree directly on the guest heap and the guest
  takes it with one `Box::from_raw`.

## Running

```
./build.sh
```

Needs the `riscv64gc-unknown-linux-gnu` Rust target
(`rustup target add riscv64gc-unknown-linux-gnu`) and a RISC-V cross-linker;
`guest/.cargo/config.toml` names `riscv64-linux-gnu-gcc-12`, overridable with
`CARGO_TARGET_RISCV64GC_UNKNOWN_LINUX_GNU_LINKER`.

Both directions are measured for three workload shapes, each 2000 iterations x 7
rounds, reported as the median round. Every run verifies what arrived: both sides
build the same tree from the same description, each path returns a checksum of the
tree it received, and all four must agree. The guest heap is accounted for at the
end, so a path that is fast because it leaked is reported as a failure, not a win.

## What the guest declares

Ordinary Rust, with two attributes on it:

```rust
#[repr(C, u64)]
pub enum Value {
    Int(i64), Float(f64), Vec3([f32; 3]), Vec4([f32; 4]),
    DVec3([f64; 3]), DVec2([f64; 2]), Str(String), Bool(bool),
    Group(Box<Attributes>), List(Box<Vec<Value>>),
}

#[repr(C)] pub struct Attr { pub key: String, pub value: Value }
#[repr(C)] pub struct Attributes { entries: Vec<Attr> }   // sorted by key
```

and the host mirror is four declarations and one registration line
(`host/src/rustattrs.hpp`). The `#[repr(C, u64)]` is the whole trick: the layout
of a Rust enum is normally reordered and niche-packed, and could not be read from
the outside at all — `repr(C, u64)` *requests* a tag followed by the payload,
which is what `GuestRustEnum` mirrors.

A sorted `Vec` rather than a `HashMap`, on purpose: `std::HashMap` is hashbrown,
whose table layout is `#[repr(Rust)]` and whose default hasher is seeded per
process, so mirroring it would mean betting on both. A guest that wants a real
`HashMap` is one `.collect()` away.

## What it measures

| Column | Meaning |
|---|---|
| `ns/op` | Wall time for one whole transfer, host work included |
| `guest instr/op` | Emulated RISC-V instructions retired |
| `guest allocs` | Calls into the guest heap (the host's arena) |

The two directions are measured differently, and deliberately so:

- **guest -> host** loops inside the guest, so no vmcall overhead is folded into
  the per-operation cost.
- **host -> guest** is one vmcall per operation, because the host has to prepare
  the argument each time. The bare vmcall baseline is printed first so it can be
  subtracted; it is a rounding error at these sizes.

## Example output

On a Ryzen 7950X, interpreted (no binary translation):

```
Bare vmcall baseline: 5 ns  (2000 iterations x 7 rounds, median)

=== Workload 'nested': 8 scalars, 6 strings of 32 bytes, 3 groups of 8, 2 lists of 12 ===

 guest -> host (the script sends attributes to the engine)
                              ns/op guest instr/op  guest allocs
  flat nodes                   8233          27562           6.0
  zero-copy                    3408              4           0.0
  zero-copy wins by           2.42x       6884.48x          none

 host -> guest (the engine hands attributes to a script)
                              ns/op guest instr/op  guest allocs
  flat nodes                  60263         506144         155.0
  zero-copy                   34023         182717          81.0
  zero-copy wins by           1.77x          2.77x         1.91x
```

**Going out, the guest stops working entirely.** Handing over the address of its
own three words is *four* instructions no matter how big the tree is, against
4.4k–27k for building the flat array — and the flat array's cost is emulated code,
where guest instructions are 10–50x more expensive than the host's. What time
remains is the host's own walk, which it would have had to do either way.

**Coming in, the guest heap traffic halves.** The flat path allocates a key block
per entry, a byte block per string, an array per group and per list, and then the
guest allocates its own `String` for every key and every string while rebuilding —
so every key is paid for twice. The zero-copy path allocates the tree once, in its
final shape, and the guest takes it with one `Box::from_raw`. The `guest instr/op`
column tracks the allocation count closely because the native heap charges the
instruction counter for the host-side work its syscalls do.

The wall-clock win is smaller than the instruction win because the host does the
same amount of real work in both cases: comparing keys, allocating guest blocks,
copying string bytes.

These numbers land within a few percent of the C++ guest's on the same machine
(155/81 allocations for `nested`, against C++'s 155/81), which is the interesting
part: a sorted `Vec` of `#[repr(C)]` entries and a `#[repr(C, u64)]` enum cost the
same as an `unordered_map` of `std::variant`, without hashbrown's `#[repr(Rust)]`
table or libstdc++'s hash-caching rule having to be mirrored.

## Notes on the setup

- The **shared heap** is what makes any of this possible: the guest installs a
  `#[global_allocator]` that ecalls the host's native heap
  (`guest/src/env.rs`), so a tree the host builds in guest memory can be owned
  and dropped by the guest. It is also why `Box::from_raw` on a host allocation is
  sound rather than merely convenient — the block really did come from the
  allocator that will release it.
- **The variant order is the ABI.** The host mirror in `rustattrs.hpp` lists the
  same ten types in the same order as the guest's `enum Value`. A change on either
  side is caught by the checksum.
- **The layout is checked before anything is measured.** `bench_probe_layout` in
  the guest reports its own view of the layout — sizes, discriminants, payload
  offsets, field order — and `host/src/probe.cpp` compares all 23 words against
  the host mirror. A rustc release that lays out `#[repr(C, u64)]` differently
  fails there instead of silently reading garbage.
- **No teardown function for the zero-copy path.** `free()` on the host mirror
  walks the whole tree through the `free_guest_object` dispatch, the host-side
  counterpart of Rust's drop glue — and in the host->guest direction it is not
  even needed, because the guest's own drop glue does it.
- **The flat path copies, on purpose.** The guest could adopt the host's key and
  string blocks with `String::from_raw_parts` instead of copying them, since the
  arena is its allocator; doing so closes the allocation gap entirely and leaves a
  ~1.2–1.4x wall-clock difference. It is not the baseline here because a path that
  adopts the sender's allocations is no longer marshalling anything, which is the
  thing being measured against.
- **Keys travel as `(pointer, length)`** in the flat format, because a Rust
  `String` is not NUL-terminated and making one would cost an allocation per key.
  The C++ benchmark's flat form carries its keys the same way, so the two are
  directly comparable.
- The workload shapes are defined twice — `guest/src/workload.rs` and
  `host/src/attributes.hpp` — because there is no header two languages can share.
  Nothing enforces that they agree except the checksum comparison that runs before
  every measurement, which is exactly where that kind of divergence shows up.
