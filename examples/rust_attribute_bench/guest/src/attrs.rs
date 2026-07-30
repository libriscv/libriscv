//! The guest-side attribute tree: ordinary Rust, with two attributes on it.
//!
//! `#[repr(C, u64)]` asks for the tagged-union layout that the host mirrors
//! with `GuestRustEnum` -- a plain `#[repr(Rust)]` enum would be reordered and
//! niche-packed, and could not be read from the outside at all. `#[repr(C)]` on
//! the entry struct does the same for the field order, which a tuple could not
//! promise.
//!
//! The entries are kept sorted by key, so a lookup is a binary search. That is
//! not a marshalling compromise: it is one allocation for the whole map instead
//! of a node per key, it needs no hasher that both sides have to agree on, and
//! `std::HashMap` is one `.collect()` away for a guest that wants one. Keys
//! compare as bytes on both sides, so the host and the guest agree on the order.

/// The leaf types, in ABI order. The discriminant of each variant is its
/// position in this list, and the host mirror in `rustattrs.hpp` lists the same
/// types in the same order -- that order IS the ABI.
#[repr(C, u64)]
pub enum Value {
    Int(i64),
    Float(f64),
    Vec3([f32; 3]),
    Vec4([f32; 4]),
    DVec3([f64; 3]),
    DVec2([f64; 2]),
    Str(String),
    Bool(bool),
    Group(Box<Attributes>),
    List(Box<Vec<Value>>),
}

pub const TAG_INT: u32 = 0;
pub const TAG_FLOAT: u32 = 1;
pub const TAG_VEC3: u32 = 2;
pub const TAG_VEC4: u32 = 3;
pub const TAG_DVEC3: u32 = 4;
pub const TAG_DVEC2: u32 = 5;
pub const TAG_STR: u32 = 6;
pub const TAG_BOOL: u32 = 7;
pub const TAG_GROUP: u32 = 8;
pub const TAG_LIST: u32 = 9;

/// The discriminant of a value, as a number. Reading the tag through the
/// pointer would work too, but a match cannot go out of sync with the enum.
pub fn tag_of(value: &Value) -> u32 {
    match value {
        Value::Int(_) => TAG_INT,
        Value::Float(_) => TAG_FLOAT,
        Value::Vec3(_) => TAG_VEC3,
        Value::Vec4(_) => TAG_VEC4,
        Value::DVec3(_) => TAG_DVEC3,
        Value::DVec2(_) => TAG_DVEC2,
        Value::Str(_) => TAG_STR,
        Value::Bool(_) => TAG_BOOL,
        Value::Group(_) => TAG_GROUP,
        Value::List(_) => TAG_LIST,
    }
}

/// One entry: a key and a value. A tuple would have been the obvious choice,
/// but its layout is unspecified and the compiler may reorder the fields.
#[repr(C)]
pub struct Attr {
    pub key: String,
    pub value: Value,
}

/// The tree itself: a newtype over a `Vec` of entries, sorted by key.
#[repr(C)]
pub struct Attributes {
    pub entries: Vec<Attr>,
}

impl Attributes {
    pub fn new() -> Self {
        Attributes {
            entries: Vec::new(),
        }
    }

    /// Adopt entries that are already in key order, which is what rebuilding
    /// from a flat array produces after one sort.
    pub fn from_sorted(entries: Vec<Attr>) -> Self {
        Attributes { entries }
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }

    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    pub fn get(&self, key: &str) -> Option<&Value> {
        self.entries
            .binary_search_by(|a| a.key.as_str().cmp(key))
            .ok()
            .map(|i| &self.entries[i].value)
    }

    /// Insert an entry in sorted position, overwriting an existing key.
    pub fn set(&mut self, key: &str, value: Value) {
        match self.entries.binary_search_by(|a| a.key.as_str().cmp(key)) {
            Ok(i) => self.entries[i].value = value,
            Err(i) => self.entries.insert(
                i,
                Attr {
                    key: key.to_string(),
                    value,
                },
            ),
        }
    }

    pub fn iter(&self) -> impl Iterator<Item = (&str, &Value)> {
        self.entries.iter().map(|a| (a.key.as_str(), &a.value))
    }
}

// --- Checksum ---------------------------------------------------------------
//
// The host computes the same number over the tree it received, which is what
// proves the two marshalling paths delivered the same data. It is
// order-independent -- every entry contributes a hash and the hashes are summed
// -- because the guest keeps its entries sorted while the host's own container
// stays in insertion order.

const BYTES_SEED: u64 = 0x9e37_79b9_7f4a_7c15;
const KEY_SEED: u64 = 0x517c_c1b7_2722_0a95;

pub fn mix64(mut v: u64) -> u64 {
    v ^= v >> 33;
    v = v.wrapping_mul(0xff51_afd7_ed55_8ccd);
    v ^= v >> 33;
    v = v.wrapping_mul(0xc4ce_b9fe_1a85_ec53);
    v ^ (v >> 33)
}

pub fn hash_bytes(data: &[u8], seed: u64) -> u64 {
    let mut hash = seed ^ (data.len() as u64).wrapping_mul(0x100_0000_01b3);
    for &byte in data {
        hash = mix64(hash ^ u64::from(byte));
    }
    hash
}

/// Hash the raw bytes of a plain leaf, which is what the host does to the same
/// bytes of the same layout.
fn hash_pod<T>(value: &T) -> u64 {
    let bytes = unsafe {
        core::slice::from_raw_parts(
            value as *const T as *const u8,
            core::mem::size_of::<T>(),
        )
    };
    hash_bytes(bytes, BYTES_SEED)
}

/// Hash one value: its discriminant, mixed with whatever it holds.
pub fn value_hash(value: &Value) -> u64 {
    let payload = match value {
        Value::Int(i) => hash_pod(i),
        Value::Float(f) => hash_pod(f),
        Value::Vec3(v) => hash_pod(v),
        Value::Vec4(v) => hash_pod(v),
        Value::DVec3(v) => hash_pod(v),
        Value::DVec2(v) => hash_pod(v),
        Value::Str(s) => hash_bytes(s.as_bytes(), BYTES_SEED),
        Value::Bool(b) => mix64(if *b { 3 } else { 5 }),
        Value::Group(group) => checksum(group),
        Value::List(list) => {
            let mut hash = 0u64;
            for element in list.iter() {
                hash = mix64(hash) ^ value_hash(element);
            }
            hash
        }
    };
    mix64(u64::from(tag_of(value)) + 1) ^ payload
}

pub fn checksum(attrs: &Attributes) -> u64 {
    let mut total = 0u64;
    for (key, value) in attrs.iter() {
        total = total.wrapping_add(hash_bytes(key.as_bytes(), KEY_SEED) ^ value_hash(value));
    }
    total
}
