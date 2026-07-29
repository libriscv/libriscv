//! The host API, as a Rust library.
//!
//! Everything in this crate is generated: build.rs runs `generate.py` over
//! `host_functions.json`, and the result is included below. What comes out is
//!
//!   * `raw` - the `extern "C"` declarations, one per JSON entry,
//!   * a `global_asm!` stub per function, and the `dyncall_table` the host
//!     resolves against at load time,
//!   * one module per namespace (`math`, `io`, `game`, `data`, `rpc`) holding
//!     safe wrappers, so that the program can call `io::print("hello")`
//!     instead of juggling pointers and lengths.
//!
//! Read the generated file itself to see the whole API at once:
//!
//! ```text
//! target/riscv64gc-unknown-linux-gnu/release/build/host_functions-*/out/host_functions.rs
//! ```

// Generated code: the JSON decides which functions exist and what they are
// called, and not every one of them is used by every program
#![allow(dead_code, unused_imports, non_camel_case_types)]
// A `String` or a `Vec<T>` in an extern block is exactly what this example is
// for. The layout is not stabilized by the language, but the host mirrors it
// (GuestRustString, GuestRustVec) and tests/unit/native_rust.cpp asks the
// guest itself to confirm the field order on every run.
#![allow(improper_ctypes)]

include!(concat!(env!("OUT_DIR"), "/host_functions.rs"));
