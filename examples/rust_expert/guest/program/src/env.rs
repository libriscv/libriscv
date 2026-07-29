//! The two things a libriscv guest has to bring itself: a heap that the host
//! shares, and an exit function.
//!
//! The global allocator is the important one. Rust's `String` and `Vec` go
//! through it for every allocation, so pointing it at the host's arena is
//! what lets the host build a `String` the guest can grow and drop, and lets
//! the guest return a `Vec` the host can read and free. Both sides end up
//! calling the same malloc.

use core::alloc::{GlobalAlloc, Layout};
use core::arch::asm;

/// Matches HEAP_SYSCALLS_BASE in the host's script.hpp
const HEAP_SYSCALLS_BASE: i32 = 490;

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

/// Where a VM call returns to, and where main() ends up.
///
/// The host sets `default_exit_function = "fast_exit"`, which makes this
/// address the return address of every vmcall: reaching it stops the machine
/// with everything still in place, ready for the next call. It is a little
/// faster than the exit page libriscv installs by default.
#[no_mangle]
pub extern "C" fn fast_exit(_code: i32) -> ! {
    loop {
        unsafe { asm!("wfi") };
    }
}
