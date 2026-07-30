//! The baseline: the tree crosses the boundary as a flat array of
//! self-describing nodes, one per key.
//!
//! Going out, the guest builds the array -- one heap allocation for the root and
//! one for every nested group and list -- and the host walks it node by node.
//! Coming in, the host builds the array in guest memory, allocating a key per
//! entry and a byte block per string value, and the guest rebuilds the tree from
//! it and releases the whole scaffolding again.
//!
//! Keys and string values travel as `(pointer, length)`, which for a Rust guest is
//! the only sensible choice -- a `String` is not NUL-terminated, and making one
//! would cost an allocation per key. The C++ version of this benchmark carries its
//! keys the same way, so the two flat formats are the same shape.

use crate::attrs::*;

/// A `(pointer, length)` pair, used for a string's bytes and for a nested node
/// array alike. Kept as `usize` so the union stays `Copy`.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct Span {
    pub ptr: usize,
    pub len: usize,
}

/// The value of one node: either the leaf inline, or a span pointing at
/// separately allocated storage.
#[repr(C)]
pub union Payload {
    pub i: i64,
    pub f: f64,
    pub v3: [f32; 3],
    pub v4: [f32; 4],
    pub d3: [f64; 3],
    pub d2: [f64; 2],
    pub b: bool,
    /// The bytes of a string, or the nodes of a group or a list
    pub span: Span,
}

/// One node of the flat representation. 40 bytes, and the host mirror in
/// `flat.hpp` says the same.
#[repr(C)]
pub struct FlatAttr {
    pub key: *const u8,
    pub key_len: u32,
    pub tag: u32,
    pub payload: Payload,
}

/// An owned flat array, together with every nested array it points at.
/// Dropping it releases all of them, which is the work the C++ `HostAttr`
/// destructor did by hand.
pub struct FlatArray {
    ptr: *mut FlatAttr,
    len: usize,
}

impl FlatArray {
    /// Flatten a tree. The nodes borrow the tree's keys and string bytes, so the
    /// tree has to outlive the array.
    pub fn build(attrs: &Attributes) -> FlatArray {
        let (ptr, len) = build_nodes(attrs);
        FlatArray { ptr, len }
    }

    pub fn as_ptr(&self) -> *const FlatAttr {
        self.ptr
    }

    pub fn len(&self) -> usize {
        self.len
    }
}

impl Drop for FlatArray {
    fn drop(&mut self) {
        unsafe { free_nodes(self.ptr, self.len) }
    }
}

fn build_nodes(attrs: &Attributes) -> (*mut FlatAttr, usize) {
    // The exact capacity, so into_boxed_slice() below neither reallocates nor
    // copies: one allocation per level, like the C++ new[]
    let mut nodes: Vec<FlatAttr> = Vec::with_capacity(attrs.len());
    for (key, value) in attrs.iter() {
        nodes.push(FlatAttr {
            key: key.as_ptr(),
            key_len: key.len() as u32,
            tag: tag_of(value),
            payload: payload_of(value),
        });
    }
    leak_nodes(nodes)
}

/// A list is the same array of nodes, with no keys.
fn build_list_nodes(list: &[Value]) -> (*mut FlatAttr, usize) {
    let mut nodes: Vec<FlatAttr> = Vec::with_capacity(list.len());
    for value in list {
        nodes.push(FlatAttr {
            key: core::ptr::null(),
            key_len: 0,
            tag: tag_of(value),
            payload: payload_of(value),
        });
    }
    leak_nodes(nodes)
}

fn payload_of(value: &Value) -> Payload {
    match value {
        Value::Int(i) => Payload { i: *i },
        Value::Float(f) => Payload { f: *f },
        Value::Vec3(v) => Payload { v3: *v },
        Value::Vec4(v) => Payload { v4: *v },
        Value::DVec3(v) => Payload { d3: *v },
        Value::DVec2(v) => Payload { d2: *v },
        Value::Bool(b) => Payload { b: *b },
        // Borrowed straight out of the tree's own String, no copy
        Value::Str(s) => Payload {
            span: Span {
                ptr: s.as_ptr() as usize,
                len: s.len(),
            },
        },
        Value::Group(group) => {
            let (ptr, len) = build_nodes(group);
            Payload {
                span: Span {
                    ptr: ptr as usize,
                    len,
                },
            }
        }
        Value::List(list) => {
            let (ptr, len) = build_list_nodes(list);
            Payload {
                span: Span {
                    ptr: ptr as usize,
                    len,
                },
            }
        }
    }
}

/// Hand the array over as a raw pointer. `free_nodes()` is what takes it back.
fn leak_nodes(nodes: Vec<FlatAttr>) -> (*mut FlatAttr, usize) {
    let len = nodes.len();
    let mut boxed = nodes.into_boxed_slice();
    let ptr = boxed.as_mut_ptr();
    core::mem::forget(boxed);
    (ptr, len)
}

/// Release a node array and, recursively, the arrays its nodes point at. The
/// key and string spans are borrowed, so they are not touched.
unsafe fn free_nodes(ptr: *mut FlatAttr, len: usize) {
    if len == 0 {
        return; // An empty boxed slice never allocated
    }
    for node in core::slice::from_raw_parts(ptr, len) {
        if node.tag == TAG_GROUP || node.tag == TAG_LIST {
            free_nodes(node.payload.span.ptr as *mut FlatAttr, node.payload.span.len);
        }
    }
    drop(Box::from_raw(core::ptr::slice_from_raw_parts_mut(ptr, len)));
}

// --- host -> guest ----------------------------------------------------------

/// Rebuild a tree from a flat array the host wrote into guest memory.
///
/// Consumes it: every key, every string value and every node array was allocated
/// by the host out of this heap, and is released here once its contents have been
/// copied into the tree.
///
/// # Safety
/// `nodes` must be an array of `count` well-formed nodes, allocated by the host
/// out of the guest arena, and not otherwise referenced.
pub unsafe fn from_flat(nodes: *const FlatAttr, count: usize) -> Attributes {
    let mut entries: Vec<Attr> = Vec::with_capacity(count);
    if count != 0 {
        for node in core::slice::from_raw_parts(nodes, count) {
            entries.push(Attr {
                key: copy_string(node.key, node.key_len as usize),
                value: read_value(node),
            });
        }
        // The array came out of the host's arena, which is this heap
        drop(Vec::from_raw_parts(nodes as *mut FlatAttr, count, count));
    }
    // The host sends its entries in its own order, and the tree is sorted
    entries.sort_unstable_by(|a, b| a.key.cmp(&b.key));
    Attributes::from_sorted(entries)
}

unsafe fn read_value(node: &FlatAttr) -> Value {
    match node.tag {
        TAG_INT => Value::Int(node.payload.i),
        TAG_FLOAT => Value::Float(node.payload.f),
        TAG_VEC3 => Value::Vec3(node.payload.v3),
        TAG_VEC4 => Value::Vec4(node.payload.v4),
        TAG_DVEC3 => Value::DVec3(node.payload.d3),
        TAG_DVEC2 => Value::DVec2(node.payload.d2),
        TAG_BOOL => Value::Bool(node.payload.b),
        TAG_STR => Value::Str(copy_string(
            node.payload.span.ptr as *const u8,
            node.payload.span.len,
        )),
        TAG_GROUP => Value::Group(Box::new(from_flat(
            node.payload.span.ptr as *const FlatAttr,
            node.payload.span.len,
        ))),
        TAG_LIST => Value::List(Box::new(read_list(
            node.payload.span.ptr as *const FlatAttr,
            node.payload.span.len,
        ))),
        _ => panic!("flat attributes: unknown value type"),
    }
}

unsafe fn read_list(nodes: *const FlatAttr, count: usize) -> Vec<Value> {
    let mut list: Vec<Value> = Vec::with_capacity(count);
    if count != 0 {
        for node in core::slice::from_raw_parts(nodes, count) {
            list.push(read_value(node));
        }
        drop(Vec::from_raw_parts(nodes as *mut FlatAttr, count, count));
    }
    list
}

/// Copy a block of bytes the host allocated, and release the block.
///
/// This is what marshalling costs, and it is where the flat path pays for every
/// key twice: once when the host allocates and writes it, and again when the
/// guest allocates its own `String` for the tree. A shared heap does allow the
/// shortcut -- `String::from_raw_parts(ptr, len, len)` adopts the host's block
/// instead, since the arena really is the allocator that will release it -- but
/// then the flat path is no longer marshalling anything, which is the thing being
/// measured against.
///
/// The host is the engine, so its bytes are trusted to be UTF-8, the same way the
/// zero-copy path trusts the `String`s the host writes directly.
unsafe fn copy_string(ptr: *const u8, len: usize) -> String {
    if len == 0 {
        return String::new();
    }
    let borrowed = core::str::from_utf8_unchecked(core::slice::from_raw_parts(ptr, len));
    let owned = borrowed.to_string();
    // The block came out of the host's arena, which is this heap
    drop(Vec::from_raw_parts(ptr as *mut u8, len, len));
    owned
}
