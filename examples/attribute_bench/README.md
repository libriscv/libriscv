# Attribute marshalling benchmark

Measures what it costs to move a structured, dynamically-typed object (a tree
of named attributes) across the guest/host boundary, comparing two strategies:

- **flat `HostAttr`**: the tree is serialized into an array of self-describing
  nodes, one per key, with strings and nested groups in separate allocations. The
  receiving side walks the array and rebuilds the tree.
- **zero-copy**: nothing is serialized. The guest hands over the address of its
  own `std::unordered_map` and the host walks it in place with
  `riscv::GuestStdUnorderedMap` / `GuestStdVariant` / `GuestStdString`; going the
  other way, the host builds the guest's map directly on the guest heap, and the
  guest takes ownership of it with a move.

## Running

```
./build.sh
```

Cross-compiles the guest with `riscv64-unknown-elf-g++`, builds the host, and
runs the benchmark. Both directions are measured for three workload shapes, each
2000 iterations x 7 rounds, reported as the median round.

Every run also verifies what arrived: both sides build the same tree from the
same description, each path returns a checksum of the tree it received, and all
four must agree with the reference. The guest heap is accounted for at the end,
so a path that is fast because it leaked is reported as a failure, not a win.

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
  flat HostAttr                7109          26460           6.0
  zero-copy                    3553              7           0.0
  zero-copy wins by           2.00x       3775.96x          none

 host -> guest (the engine hands attributes to a script)
                              ns/op guest instr/op  guest allocs
  flat HostAttr               70719         511827         155.0
  zero-copy                   34763         179630          81.0
  zero-copy wins by           2.03x          2.85x         1.91x
```

Roughly a 2x wall-clock win in both directions across all three shapes, but the
interesting columns are the other two.

**Going out, the guest stops working entirely.** Handing over an address is seven
instructions no matter how big the tree is, against 4.3k-26k for building the
flat array -- and the flat array's cost is emulated code, which is where guest
instructions are 10-50x more expensive than the host's. What time remains is the
host's own walk, which it would have had to do either way.

**Coming in, the guest heap traffic halves.** The flat path allocates a key
string per entry, a value string per string, an array per group and per list, and
then the guest allocates the whole tree *again* while re-inserting -- so every key
is paid for twice. The zero-copy path allocates the tree once, in its final shape,
and the guest takes it with a move.

The wall-clock win is smaller than the instruction win because the host does the
same amount of real work in both cases: hashing keys, allocating guest nodes,
copying string bytes.

## Notes on the setup

- The **native heap** (`setup_native_heap`) is what makes any of this possible:
  the guest's `malloc`/`free` are the host's arena, so a tree the host builds in
  guest memory can be owned and freed by the guest.
- The guest map uses a **custom hasher**, which means libstdc++ does not cache the
  hash code in its nodes. The host mirror must say so
  (`GuestStdUnorderedMap<..., false>`) or every key and value shifts.
- The order of the `std::variant` alternatives **is the ABI**, and the host
  mirror in `zerocopy.hpp` must match the guest's declaration exactly. A layout
  change on either side is caught by the checksum check.
- Nested groups and lists are `std::unique_ptr` alternatives, which libriscv sees
  as opaque pointer-sized values; the walk follows them explicitly, and so does
  the free. That is why `destroyGuestAttributes` exists rather than just calling
  `free()` on the map.
- The host-side container is a plain vector of entries rather than a hash map, to
  keep the host's own bookkeeping identical and cheap for both paths.
