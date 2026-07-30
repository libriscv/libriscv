//! The shapes of the attribute trees the benchmark moves, and the one function
//! that builds them.
//!
//! The host has its own copy of this file (`host/src/attributes.hpp`), because
//! the two sides are written in different languages and there is no header to
//! share. Nothing enforces that they agree -- except that both sides build the
//! tree and compare checksums before any number is reported, which is exactly
//! the kind of divergence that would show up there first.

use crate::attrs::{Attributes, Value};

/// The shape of one attribute tree. The three presets below stand in for the
/// payloads an engine actually ships across the boundary.
pub struct Workload {
    /// Only the harness prints it, but it keeps the presets below readable and
    /// the struct the same shape as the host's
    #[allow(dead_code)]
    pub name: &'static str,
    /// Keys cycling through the numeric/bool leaf types
    pub scalars: usize,
    /// String-valued keys
    pub strings: usize,
    /// Bytes per string
    pub string_len: usize,
    /// Nested sub-groups
    pub groups: usize,
    /// Keys per sub-group
    pub group_size: usize,
    /// List-valued keys
    pub lists: usize,
    /// Elements per list
    pub list_len: usize,
}

/// An RPC payload: a handful of scalars and one short string.
pub const SMALL: Workload = Workload {
    name: "small",
    scalars: 7,
    strings: 1,
    string_len: 12,
    groups: 0,
    group_size: 0,
    lists: 0,
    list_len: 0,
};
/// A mail letter: mostly strings.
pub const STRINGS: Workload = Workload {
    name: "strings",
    scalars: 6,
    strings: 10,
    string_len: 48,
    groups: 0,
    group_size: 0,
    lists: 0,
    list_len: 0,
};
/// A quest/shop state blob: groups and lists on top of the leaves.
pub const NESTED: Workload = Workload {
    name: "nested",
    scalars: 8,
    strings: 6,
    string_len: 32,
    groups: 3,
    group_size: 8,
    lists: 2,
    list_len: 12,
};

pub fn workload_at(index: i32) -> Option<&'static Workload> {
    match index {
        0 => Some(&SMALL),
        1 => Some(&STRINGS),
        2 => Some(&NESTED),
        _ => None,
    }
}

fn repeated(byte: u8, len: usize) -> String {
    // ASCII, so the bytes are valid UTF-8 by construction
    unsafe { String::from_utf8_unchecked(vec![byte; len]) }
}

/// Fill a tree with the given shape. The host's `build_workload()` does the
/// same, key for key and value for value.
pub fn build_workload(attrs: &mut Attributes, w: &Workload) {
    for i in 0..w.scalars {
        let value = match i % 6 {
            0 => Value::Int(i as i64 * 1337),
            1 => Value::Float(i as f64 * 0.5),
            2 => Value::Vec3([i as f32, 1.0, 2.0]),
            3 => Value::Vec4([i as f32, 1.0, 2.0, 3.0]),
            4 => Value::DVec3([i as f64, 1.5, 2.5]),
            _ => Value::Bool((i & 1) != 0),
        };
        attrs.set(&format!("s{}", i), value);
    }
    for i in 0..w.strings {
        attrs.set(
            &format!("t{}", i),
            Value::Str(repeated(b'a' + (i % 26) as u8, w.string_len)),
        );
    }
    for g in 0..w.groups {
        let mut group = Attributes::new();
        for i in 0..w.group_size {
            let value = if i % 2 == 0 {
                Value::Int(g as i64 * 100 + i as i64)
            } else {
                Value::Str(repeated(b'A' + (i % 26) as u8, w.string_len))
            };
            group.set(&format!("k{}", i), value);
        }
        attrs.set(&format!("g{}", g), Value::Group(Box::new(group)));
    }
    for l in 0..w.lists {
        let mut list: Vec<Value> = Vec::with_capacity(w.list_len);
        for i in 0..w.list_len {
            list.push(match i % 3 {
                0 => Value::Int(i as i64),
                1 => Value::Float(i as f64 * 0.25),
                _ => Value::Str(repeated(b'x', w.string_len)),
            });
        }
        attrs.set(&format!("l{}", l), Value::List(Box::new(list)));
    }
}
