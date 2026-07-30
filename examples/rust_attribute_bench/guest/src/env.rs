//! The three things a libriscv guest has to bring itself: a heap that the host
//! shares, the block memory operations, and an exit function.
//!
//! The global allocator is what the whole benchmark rests on. Every `String`,
//! `Vec` and `Box` in this program is allocated out of the host's arena, so a
//! tree the host builds is a tree the guest can own and drop, and a tree the
//! guest builds is one the host can read and free. Neither side has to know
//! which of them allocated what.
//!
//! The `__wrap_mem*` functions are the same idea applied to bulk memory, and
//! they matter here for a second reason: the C++ guest this benchmark is
//! compared against has always had them, so without them the numbers would be
//! measuring the accelerated syscalls rather than the marshalling strategy.

use core::alloc::{GlobalAlloc, Layout};
use core::arch::asm;

/// Matches the native heap base in the host's script.hpp
const HEAP_SYSCALLS_BASE: i32 = 490;

/// Matches MEMORY_SYSCALLS_BASE in the host's script.hpp
const MEMORY_SYSCALLS_BASE: i32 = 495;

/// An allocator that forwards to `machine.setup_native_heap()` on the host.
/// The arena hands out 16-byte aligned blocks, which covers every alignment
/// the standard collections ask for.
struct ArenaAllocator;

unsafe impl GlobalAlloc for ArenaAllocator {
    #[inline]
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let ret: *mut u8;
        asm!("ecall", in("a7") HEAP_SYSCALLS_BASE + 0,
            in("a0") layout.size(), in("a1") layout.align(), lateout("a0") ret);
        ret
    }
    #[inline]
    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        let ret: *mut u8;
        asm!("ecall", in("a7") HEAP_SYSCALLS_BASE + 1,
            in("a0") 1, in("a1") layout.size(), lateout("a0") ret);
        ret
    }
    #[inline]
    unsafe fn realloc(&self, ptr: *mut u8, _layout: Layout, new_size: usize) -> *mut u8 {
        let ret: *mut u8;
        asm!("ecall", in("a7") HEAP_SYSCALLS_BASE + 2,
            in("a0") ptr, in("a1") new_size, lateout("a0") ret);
        ret
    }
    #[inline]
    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        asm!("ecall", in("a7") HEAP_SYSCALLS_BASE + 3,
            in("a0") ptr, lateout("a0") _);
    }
}

#[global_allocator]
static ALLOCATOR: ArenaAllocator = ArenaAllocator;

// ---------------------------------------------------------------------------
// Block memory operations, handed to the host.
//
// A tree of Strings and Vecs spends most of its bulk memory work inside
// memcpy, without ever naming it: every RawVec growth, every String clone and
// every push_str ends up there. The link step passes --wrap=memcpy and
// friends, which rewrites every reference in the program - Rust's own and the
// ones inside glibc - to these stubs, so the host does the copying with a
// native memcpy instead of the guest emulating the loop.
//
// The bodies are pure inline assembly on purpose: a hand-written byte loop
// would be recognised by LLVM as a memcpy and turned back into a call to this
// very function.
// ---------------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn __wrap_memcpy(dest: *mut u8, src: *const u8, n: usize) -> *mut u8 {
    asm!("ecall", in("a7") MEMORY_SYSCALLS_BASE + 0,
        in("a0") dest, in("a1") src, in("a2") n, lateout("a0") _);
    dest
}

#[no_mangle]
pub unsafe extern "C" fn __wrap_memset(dest: *mut u8, value: i32, n: usize) -> *mut u8 {
    asm!("ecall", in("a7") MEMORY_SYSCALLS_BASE + 1,
        in("a0") dest, in("a1") value, in("a2") n, lateout("a0") _);
    dest
}

#[no_mangle]
pub unsafe extern "C" fn __wrap_memmove(dest: *mut u8, src: *const u8, n: usize) -> *mut u8 {
    asm!("ecall", in("a7") MEMORY_SYSCALLS_BASE + 2,
        in("a0") dest, in("a1") src, in("a2") n, lateout("a0") _);
    dest
}

#[no_mangle]
pub unsafe extern "C" fn __wrap_memcmp(s1: *const u8, s2: *const u8, n: usize) -> i32 {
    let result: i32;
    asm!("ecall", in("a7") MEMORY_SYSCALLS_BASE + 3,
        in("a0") s1, in("a1") s2, in("a2") n, lateout("a0") result);
    result
}

// Comparing two slices with `==` does not call memcmp: LLVM knows the caller
// only wants to know whether they differ, and emits the equality-only entry
// point instead - `__memcmpeq` on a current toolchain, `bcmp` on an older one.
// Both are satisfied by a memcmp result, since all either one promises is zero
// or non-zero. This benchmark compares attribute keys constantly, so leaving
// them out would leave most of the comparing emulated.

#[no_mangle]
pub unsafe extern "C" fn __wrap___memcmpeq(s1: *const u8, s2: *const u8, n: usize) -> i32 {
    __wrap_memcmp(s1, s2, n)
}

#[no_mangle]
pub unsafe extern "C" fn __wrap_bcmp(s1: *const u8, s2: *const u8, n: usize) -> i32 {
    __wrap_memcmp(s1, s2, n)
}

/// Where a VM call returns to, and where main() ends up.
///
/// The host sets `default_exit_function = "fast_exit"`, which makes this
/// address the return address of every vmcall: reaching it stops the machine
/// with everything still in place, ready for the next call.
#[no_mangle]
pub extern "C" fn fast_exit(_code: i32) -> ! {
    loop {
        unsafe { asm!("wfi") };
    }
}
