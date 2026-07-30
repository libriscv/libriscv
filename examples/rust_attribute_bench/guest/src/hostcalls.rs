//! The host functions, as plain system calls. The two attribute calls send the
//! same tree to the host; they differ only in what crosses the boundary.

use crate::flat::FlatAttr;
use core::arch::asm;

const SYSCALL_OUT_FLAT: i32 = 480;
const SYSCALL_OUT_OBJ: i32 = 481;
const SYSCALL_PRINT: i32 = 482;

/// Send a flat array of self-describing nodes.
/// @return The checksum the host computed over the tree it rebuilt.
#[inline]
pub fn attr_out_flat(nodes: *const FlatAttr, count: usize) -> u64 {
    let ret: u64;
    unsafe {
        asm!("ecall", in("a7") SYSCALL_OUT_FLAT,
            in("a0") nodes, in("a1") count, lateout("a0") ret);
    }
    ret
}

/// Send the address of the guest's own `Attributes`, and nothing else. The
/// host walks it in place.
/// @return The checksum the host computed over the tree it read.
#[inline]
pub fn attr_out_obj(attrs: *const crate::attrs::Attributes) -> u64 {
    let ret: u64;
    unsafe {
        asm!("ecall", in("a7") SYSCALL_OUT_OBJ, in("a0") attrs, lateout("a0") ret);
    }
    ret
}

pub fn print(text: &str) {
    unsafe {
        asm!("ecall", in("a7") SYSCALL_PRINT,
            in("a0") text.as_ptr(), in("a1") text.len(), lateout("a0") _);
    }
}
