//! The guest half of the benchmark.
//!
//! One reference tree is built per workload and then shipped across the boundary
//! over and over, both ways, once per marshalling strategy. Every entry point
//! returns a checksum, so the harness can prove the two strategies deliver
//! identical data -- and the guest heap is accounted for at the end, so a path
//! that is fast because it leaked is reported as a failure rather than a win.
//!
//! Nothing here is emulator-specific except `mod env`, which points the global
//! allocator at the host's arena.

// The exported functions pass Rust types through extern "C" on purpose: the host
// mirrors their layout instead of serializing them
#![allow(improper_ctypes_definitions)]

mod attrs;
mod env;
mod flat;
mod hostcalls;
mod workload;

use attrs::{checksum, Attributes};
use flat::{FlatArray, FlatAttr};

/// The tree the guest sends out, built once per workload.
///
/// A raw pointer rather than an `Option<Box<..>>` so that reaching it never
/// forms a reference to the static itself.
static mut REFERENCE: *mut Attributes = core::ptr::null_mut();

fn reference() -> &'static Attributes {
    unsafe { &*REFERENCE }
}

fn set_reference(attrs: Attributes) {
    unsafe {
        let old = REFERENCE;
        REFERENCE = Box::into_raw(Box::new(attrs));
        if !old.is_null() {
            drop(Box::from_raw(old));
        }
    }
}

#[no_mangle]
pub extern "C" fn bench_setup(index: i32) -> i64 {
    let Some(w) = workload::workload_at(index) else {
        return -1;
    };
    let mut attrs = Attributes::new();
    workload::build_workload(&mut attrs, w);
    let sum = checksum(&attrs);
    set_reference(attrs);
    sum as i64
}

// --- guest -> host ----------------------------------------------------------

/// Flatten the tree and hand the host the array, once per iteration.
#[no_mangle]
pub extern "C" fn bench_out_flat(iterations: i32) -> u64 {
    let attrs = reference();
    let mut result = 0;
    for _ in 0..iterations {
        let flat = FlatArray::build(attrs);
        result = hostcalls::attr_out_flat(flat.as_ptr(), flat.len());
    }
    result
}

/// Hand the host the address of the tree itself, once per iteration.
#[no_mangle]
pub extern "C" fn bench_out_obj(iterations: i32) -> u64 {
    let attrs = reference() as *const Attributes;
    let mut result = 0;
    for _ in 0..iterations {
        result = hostcalls::attr_out_obj(attrs);
    }
    result
}

// --- host -> guest ----------------------------------------------------------

/// Rebuild a tree from a flat array the host wrote into guest memory, which
/// also releases every allocation the host made for it.
#[no_mangle]
pub extern "C" fn bench_in_flat(nodes: *const FlatAttr, count: usize) -> u64 {
    let attrs = unsafe { flat::from_flat(nodes, count) };
    checksum(&attrs)
}

/// Take ownership of a tree the host built directly on the guest heap. One
/// `from_raw`, no rebuilding, and the drop glue on the way out walks the whole
/// tree -- which is why the host needs no teardown function for it.
#[no_mangle]
pub extern "C" fn bench_in_obj(ptr: *mut Attributes) -> u64 {
    let attrs: Box<Attributes> = unsafe { Box::from_raw(ptr) };
    checksum(&attrs)
}

/// The vmcall baseline, so the harness can report what a call costs before any
/// attributes are involved.
#[no_mangle]
pub extern "C" fn bench_noop() -> u64 {
    0
}

/// Drop the reference tree, so the harness can account for the guest heap
/// without the live tree counting as outstanding.
#[no_mangle]
pub extern "C" fn bench_teardown() {
    unsafe {
        let old = REFERENCE;
        REFERENCE = core::ptr::null_mut();
        if !old.is_null() {
            drop(Box::from_raw(old));
        }
    }
}

// --- Layout probe -----------------------------------------------------------
//
// The layout of a #[repr(C, uN)] enum is requested, not discovered, but it is
// still worth checking: a rustc release that lays it out differently should fail
// here rather than read garbage everywhere else. The host compares every word
// against its own mirror.

/// The offset of the payload of a variant, measured against the enum itself
macro_rules! payload_offset {
    ($value:expr, $pattern:pat => $field:expr) => {{
        let value = $value;
        let base = &value as *const attrs::Value as usize;
        match &value {
            $pattern => ($field as *const _ as usize) - base,
            #[allow(unreachable_patterns)]
            _ => usize::MAX,
        }
    }};
}

/// The discriminant, read as the first bytes of the enum rather than matched, so
/// that it is the ABI being measured and not the compiler's opinion of it.
fn raw_tag(value: &attrs::Value) -> usize {
    unsafe { *(value as *const attrs::Value as *const u64) as usize }
}

pub const PROBE_WORDS: usize = 23;

#[no_mangle]
pub extern "C" fn bench_probe_layout(out: *mut usize) {
    use attrs::{Attr, Attributes, Value};

    let attr = Attr {
        key: String::new(),
        value: Value::Int(1),
    };
    let attr_base = &attr as *const Attr as usize;

    let words: [usize; PROBE_WORDS] = [
        core::mem::size_of::<Value>(),
        core::mem::align_of::<Value>(),
        // The discriminants, which must be the declaration order 0..n
        raw_tag(&Value::Int(0)),
        raw_tag(&Value::Float(0.0)),
        raw_tag(&Value::Vec3([0.0; 3])),
        raw_tag(&Value::Vec4([0.0; 4])),
        raw_tag(&Value::DVec3([0.0; 3])),
        raw_tag(&Value::DVec2([0.0; 2])),
        raw_tag(&Value::Str(String::new())),
        raw_tag(&Value::Bool(false)),
        raw_tag(&Value::Group(Box::new(Attributes::new()))),
        raw_tag(&Value::List(Box::new(Vec::new()))),
        // The payload offsets, which must all be sizeof(tag)
        payload_offset!(Value::Int(1), Value::Int(x) => x),
        payload_offset!(Value::DVec3([0.0; 3]), Value::DVec3(x) => x),
        payload_offset!(Value::Str(String::new()), Value::Str(x) => x),
        payload_offset!(Value::Group(Box::new(Attributes::new())), Value::Group(x) => x),
        // The entry struct: a String followed by a Value, in that order
        core::mem::size_of::<Attr>(),
        (&attr.key as *const String as usize) - attr_base,
        (&attr.value as *const Value as usize) - attr_base,
        // Attributes is a newtype over the Vec, so it is the same three words
        core::mem::size_of::<Attributes>(),
        core::mem::size_of::<Box<Attributes>>(),
        // And the flat node, whose layout the host mirrors as well
        core::mem::size_of::<FlatAttr>(),
        core::mem::offset_of!(FlatAttr, payload),
    ];
    unsafe { core::ptr::copy_nonoverlapping(words.as_ptr(), out, words.len()) };
}

fn main() {
    hostcalls::print("Guest: ready, pausing.\n");
    // Stop here with the runtime fully initialized, so that the host makes its
    // calls into a warm guest
    env::fast_exit(0);
}
