/**
 * Rust guest collections, from both sides.
 *
 * The file is in two halves. The "Example:" tests near the top are the ones to
 * read first: each shows a single way a Rust value crosses the boundary, and
 * together they cover both directions - the host building a String or a Vec for
 * the guest, the guest returning one to the host, the guest lending one out in
 * a system call, and ownership passing either way. The tests below them are the
 * exhaustive checks that keep the host mirrors honest: field order, RawVec
 * growth, UTF-8 validation and allocation balance.
 *
 * The one thing that makes all of it work is at the top of the guest program:
 * the guest installs a #[global_allocator] that forwards to the host arena, so
 * both sides allocate out of the same heap.
 */
#include <catch2/catch_test_macros.hpp>

#include <libriscv/machine.hpp>
#include <libriscv/native_heap.hpp>
#include <libriscv/guest/guest_rust_string.hpp>
#include <libriscv/guest/guest_rust_vec.hpp>
#include <libriscv/guest/guest_rust_box.hpp>
#include <libriscv/guest/guest_rust_enum.hpp>
#include <libriscv/guest/guest_rust_attributes.hpp>

extern std::vector<uint8_t> build_rust_and_load(
	const std::string& code, const std::string& args = "-C opt-level=2");
extern bool rust_toolchain_available();

using namespace riscv;

static constexpr uint64_t MAX_INSTRUCTIONS = 100'000'000ul;
static constexpr size_t   HEAP_SIZE = 4UL << 20;

static constexpr int HEAP_SYSCALLS_BASE  = 470;
static constexpr int MEMORY_SYSCALLS_BASE= 475;
static constexpr int SYSCALL_PAUSE       = 480;
static constexpr int SYSCALL_FILL_STRING = 481;
static constexpr int SYSCALL_FILL_VEC    = 482;
static constexpr int SYSCALL_TAKE_STR    = 483;
static constexpr int SYSCALL_TAKE_VEC    = 484;
static constexpr int SYSCALL_TAKE_ATTRS  = 485;
static constexpr int SYSCALL_FILL_ATTRS  = 486;

using RustString = GuestRustString<RISCV64>;
using RustStr    = GuestRustStr<RISCV64>;
template <typename T> using RustVec   = GuestRustVec<RISCV64, T>;
template <typename T> using RustSlice = GuestRustSlice<RISCV64, T>;
template <typename T> using RustBox   = GuestRustBox<RISCV64, T>;
template <typename T> using RustBoxedSlice = GuestRustBoxedSlice<RISCV64, T>;
using RustBoxedStr = GuestRustBoxedStr<RISCV64>;
using ScopedRustString = ScopedGuestRustString<RISCV64>;
template <typename T> using ScopedRustVec = ScopedGuestRustVec<RISCV64, T>;
template <typename T> using ScopedRustBox = ScopedGuestRustBox<RISCV64, T>;

// ---------------------------------------------------------------------------
// The host mirror of the guest's attribute tree.
//
// The value type has to name itself - a group is a Box<Attributes>, and
// Attributes holds entries of values - so it is declared as a struct deriving
// from the enum, and registered with one line. The variant order and the tag
// width ARE the ABI: they must match the guest's #[repr(C, u64)] enum, and the
// layout probe further down checks that they do.
// ---------------------------------------------------------------------------

struct RustValue;
using RustAttrs = GuestRustAttributes<RISCV64, RustValue>;
using RustGroup = RustBox<RustAttrs>;
using RustList  = RustBox<RustVec<RustValue>>;

struct RustValue : GuestRustEnum<RISCV64, uint64_t,
	std::monostate,    // Nil
	int64_t,           // Int(i64)
	double,            // Float(f64)
	bool,              // Bool(bool)
	RustString,        // Str(String)
	RustGroup,         // Group(Box<Attributes>)
	RustList>          // List(Box<Vec<Value>>)
{
	using GuestRustEnum::GuestRustEnum;
};
RISCV_REGISTER_GUEST_RUST_ENUM(RISCV64, RustValue);

using ScopedRustAttrs = ScopedArenaObject<RISCV64, RustAttrs>;

// The whole point of the layout being requested rather than discovered: the
// host mirror is the same size as what the guest declared
static_assert(sizeof(RustValue) == 8 + sizeof(RustString), "tag + the widest variant");
static_assert(sizeof(GuestRustAttr<RISCV64, RustValue>)
	== sizeof(RustString) + sizeof(RustValue), "Attr is a String and a Value");
static_assert(sizeof(RustAttrs) == sizeof(RustVec<int>), "Attributes is a Vec of entries");

// What the host syscalls below write into the guests own collections
static const std::string HOST_TEXT = "A string the host wrote into a guest-owned String";
static const std::string GUEST_SUFFIX = " and the guest appended to it";

// What the guest lends out to the host in the worked examples
static const std::string LENT_TEXT = "A borrowed &str lent out by the guest";
static const std::vector<std::string> LENT_NAMES { "alpha", "beta", "gamma" };

// Where the syscall handlers below put what the guest lent them
static std::string g_lent_str;
static std::vector<std::string> g_lent_vec;

// What the guest's rust_attrs_build() puts in the tree it lends to the host
static const std::string GUEST_BETA = "a string the guest allocated";
static const std::string GUEST_NESTED = "inside the group";

// Where the attribute system calls below leave what the guest lent them
static uint64_t g_lent_attrs_checksum = 0;
static std::size_t g_lent_attrs_size = 0;

/// @brief The FNV-1a that the guest computes over the same bytes, so that both
/// sides can prove they are looking at the same string.
static uint64_t fnv1a(std::string_view bytes)
{
	uint64_t hash = 0xcbf29ce484222325ull;
	for (const unsigned char byte : bytes) {
		hash ^= byte;
		hash *= 0x100000001b3ull;
	}
	return hash;
}

// The guest program. It is a normal statically linked riscv64gc Linux program,
// except that its global allocator is the host arena: every String and Vec it
// makes is allocated with the same malloc() that GuestRustString uses, which is
// what lets the two sides hand collections back and forth.
static const std::string RUST_GUEST_PROGRAM = R"RUST(
#![allow(improper_ctypes_definitions)]
use core::alloc::{GlobalAlloc, Layout};
use core::arch::asm;

const HEAP_SYSCALLS_BASE: i32 = 470;
const MEMORY_SYSCALLS_BASE: i32 = 475;
const SYSCALL_PAUSE: i32 = 480;
const SYSCALL_FILL_STRING: i32 = 481;
const SYSCALL_FILL_VEC: i32 = 482;
const SYSCALL_TAKE_STR: i32 = 483;
const SYSCALL_TAKE_VEC: i32 = 484;
const SYSCALL_TAKE_ATTRS: i32 = 485;
const SYSCALL_FILL_ATTRS: i32 = 486;

// The arena hands out 16-byte aligned blocks, which covers every alignment
// that the standard collections ask for.
struct SysAllocator;

unsafe impl GlobalAlloc for SysAllocator {
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
static ALLOCATOR: SysAllocator = SysAllocator;

// The block memory operations, handed to the host the same way the heap is.
// The link step passes --wrap=memcpy and friends, which rewrites every
// reference to memcpy in the program - Rust's own and the ones inside glibc -
// to __wrap_memcpy, so these four take over without any calling code knowing.
// The host then copies with a native memcpy instead of emulating the loop.
//
// The bodies are pure inline assembly on purpose: a hand-written byte loop
// here would be recognised by LLVM as a memcpy and turned back into a call to
// this very function.

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

// Comparing two slices with == does not call memcmp: LLVM knows the caller
// only wants to know whether they differ, and emits the equality-only entry
// point instead - __memcmpeq on a current toolchain, bcmp on an older one.
// Both are satisfied by a memcmp result, since all either one promises is
// zero or non-zero.

#[no_mangle]
pub unsafe extern "C" fn __wrap___memcmpeq(s1: *const u8, s2: *const u8, n: usize) -> i32 {
	__wrap_memcmp(s1, s2, n)
}

#[no_mangle]
pub unsafe extern "C" fn __wrap_bcmp(s1: *const u8, s2: *const u8, n: usize) -> i32 {
	__wrap_memcmp(s1, s2, n)
}

/// The same FNV-1a that the host computes, so both sides can prove they are
/// looking at the same bytes.
fn fnv1a(bytes: &[u8]) -> u64 {
	let mut hash: u64 = 0xcbf29ce484222325;
	for b in bytes {
		hash ^= *b as u64;
		hash = hash.wrapping_mul(0x100000001b3);
	}
	hash
}

unsafe fn borrow<'a>(ptr: *const u8, len: usize) -> &'a str {
	core::str::from_utf8(core::slice::from_raw_parts(ptr, len))
		.expect("The host passed bytes that are not UTF-8")
}

// ---------------------------------------------------------------------------
// The guest half of the worked examples. Everything below this block is the
// guest half of the exhaustive tests further down in native_rust.cpp.
// ---------------------------------------------------------------------------

/// A String returned by value. It is too large for a register, so the caller
/// allocates room for it and passes the address as a hidden first argument.
#[no_mangle]
pub extern "C" fn rust_make_greeting() -> String {
	String::from("A String returned by value from the guest")
}

/// The same, but with an argument: the hidden pointer takes the first register,
/// so `count` arrives in the second one.
#[no_mangle]
pub extern "C" fn rust_make_squares(count: usize) -> Vec<u32> {
	(0..count as u32).map(|i| i * i).collect()
}

/// The guest lends the host a &str, which is two registers: address and length.
#[no_mangle]
pub extern "C" fn rust_lends_str() {
	let text = String::from("A borrowed &str lent out by the guest");
	unsafe {
		asm!("ecall", in("a7") SYSCALL_TAKE_STR,
			in("a0") text.as_ptr(), in("a1") text.len(), lateout("a0") _);
	}
	// Still owned here, and dropped at the end of the function
}

/// The guest lends the host a whole collection, which is one register holding
/// the address of its three words.
#[no_mangle]
pub extern "C" fn rust_lends_vec() {
	let names: Vec<String> = vec![
		String::from("alpha"), String::from("beta"), String::from("gamma")];
	unsafe {
		asm!("ecall", in("a7") SYSCALL_TAKE_VEC,
			in("a0") &names as *const Vec<String>, lateout("a0") _);
	}
}

/// Collections nest: every element of the outer Vec is itself the three words
/// of a Vec, which is exactly what the host pushes into it.
#[no_mangle]
pub extern "C" fn rust_nested_sum(vec: *const Vec<Vec<u32>>) -> u64 {
	let vec: &Vec<Vec<u32>> = unsafe { &*vec };
	vec.iter().map(|row| row.iter().map(|x| *x as u64).sum::<u64>()).sum()
}

// ---------------------------------------------------------------------------
// Layout probe: the raw machine words of every collection, followed by what
// the Rust API says they hold, so the host can prove the field order it uses.
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn rust_probe_layout(out: *mut usize) {
	let vec: Vec<u32> = vec![1, 2, 3];
	let string = String::from("A string that is definitely heap allocated");
	let slice: &[u32] = &vec[..];
	let strref: &str = &string[..];
	let empty_string = String::new();
	let empty_vec: Vec<u32> = Vec::new();

	unsafe {
		let vw: [usize; 3] = core::mem::transmute_copy(&vec);
		let sw: [usize; 3] = core::mem::transmute_copy(&string);
		let slw: [usize; 2] = core::mem::transmute_copy(&slice);
		let stw: [usize; 2] = core::mem::transmute_copy(&strref);
		let ew: [usize; 3] = core::mem::transmute_copy(&empty_string);
		let evw: [usize; 3] = core::mem::transmute_copy(&empty_vec);
		let words: [usize; 26] = [
			vw[0], vw[1], vw[2], vec.as_ptr() as usize, vec.len(), vec.capacity(),
			sw[0], sw[1], sw[2], string.as_ptr() as usize, string.len(), string.capacity(),
			slw[0], slw[1], slice.as_ptr() as usize, slice.len(),
			stw[0], stw[1], strref.as_ptr() as usize, strref.len(),
			ew[0], ew[1], ew[2],
			evw[0], evw[1], evw[2],
		];
		core::ptr::copy_nonoverlapping(words.as_ptr(), out, words.len());
	}
}

/// Forces the allocations that the runtime makes lazily on its first use of
/// each collection, so that the host can take an allocation baseline which
/// does not move underneath it afterwards.
#[no_mangle]
pub extern "C" fn rust_warmup() {
	let mut string = String::from("warm");
	string.push_str("up");
	let mut vec: Vec<u32> = Vec::new();
	vec.push(1);
	let strings: Vec<String> = vec![String::from("warm"), String::from("up")];
	let joined = strings.join(", ");
	core::hint::black_box((&string, &vec, &strings, &joined));
}

/// The capacity that Rust's RawVec ends up with after pushing one element at
/// a time. The host's reserve() has to arrive at the same number.
#[no_mangle]
pub extern "C" fn rust_vec_capacity_after(pushes: usize) -> usize {
	let mut vec: Vec<u32> = Vec::new();
	for i in 0..pushes {
		vec.push(i as u32);
	}
	vec.capacity()
}

#[no_mangle]
pub extern "C" fn rust_string_capacity_after(pushes: usize) -> usize {
	let mut string = String::new();
	for _ in 0..pushes {
		string.push('x');
	}
	string.capacity()
}

// The smallest allocation RawVec makes depends on the size of the element:
// 8 for a byte, 4 for anything up to 1024 bytes, and 1 above that. One
// function per branch, so the host constant can be checked against each.

#[no_mangle]
pub extern "C" fn rust_u8vec_capacity_after(pushes: usize) -> usize {
	let mut vec: Vec<u8> = Vec::new();
	for i in 0..pushes {
		vec.push(i as u8);
	}
	vec.capacity()
}

#[no_mangle]
pub extern "C" fn rust_u64vec_capacity_after(pushes: usize) -> usize {
	let mut vec: Vec<u64> = Vec::new();
	for i in 0..pushes {
		vec.push(i as u64);
	}
	vec.capacity()
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Big { pub data: [u8; 2048] }

#[no_mangle]
pub extern "C" fn rust_bigvec_capacity_after(pushes: usize) -> usize {
	let mut vec: Vec<Big> = Vec::new();
	for _ in 0..pushes {
		vec.push(Big { data: [0u8; 2048] });
	}
	vec.capacity()
}

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn rust_string_len(string: *const String) -> usize {
	let string: &String = unsafe { &*string };
	string.len()
}

#[no_mangle]
pub extern "C" fn rust_string_checksum(string: *const String) -> u64 {
	let string: &String = unsafe { &*string };
	fnv1a(string.as_bytes())
}

#[no_mangle]
pub extern "C" fn rust_string_is_utf8(ptr: *const u8, len: usize) -> i32 {
	let bytes = unsafe { core::slice::from_raw_parts(ptr, len) };
	core::str::from_utf8(bytes).is_ok() as i32
}

/// Append to a host-built string, which reallocates it in the arena.
#[no_mangle]
pub extern "C" fn rust_string_append(string: *mut String, ptr: *const u8, len: usize) {
	let string: &mut String = unsafe { &mut *string };
	string.push_str(unsafe { borrow(ptr, len) });
}

/// Build a string in the guest, at an address the host picked.
#[no_mangle]
pub extern "C" fn rust_string_make(out: *mut String, ptr: *const u8, len: usize) {
	let mut string = String::from(unsafe { borrow(ptr, len) });
	string.push_str(" (built by the guest)");
	unsafe { core::ptr::write(out, string) };
}

/// Drop a host-built string in the guest, leaving an empty one behind.
#[no_mangle]
pub extern "C" fn rust_string_drop(string: *mut String) {
	unsafe {
		core::ptr::drop_in_place(string);
		core::ptr::write(string, String::new());
	}
}

// ---------------------------------------------------------------------------
// Ownership: the guest keeps a string the host allocated
// ---------------------------------------------------------------------------

static mut STORED: Option<String> = None;

fn stored() -> &'static mut Option<String> {
	unsafe { &mut *core::ptr::addr_of_mut!(STORED) }
}

#[no_mangle]
pub extern "C" fn rust_take_string(string: *mut String) {
	let owned = unsafe { core::ptr::read(string) };
	unsafe { core::ptr::write(string, String::new()) };
	*stored() = Some(owned);
}

#[no_mangle]
pub extern "C" fn rust_stored_checksum() -> u64 {
	match stored() {
		Some(string) => fnv1a(string.as_bytes()),
		None => 0,
	}
}

#[no_mangle]
pub extern "C" fn rust_release_stored() {
	*stored() = None;
}

// ---------------------------------------------------------------------------
// Vectors
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn rust_vec_sum(vec: *const Vec<u32>) -> u64 {
	let vec: &Vec<u32> = unsafe { &*vec };
	vec.iter().map(|x| *x as u64).sum()
}

#[no_mangle]
pub extern "C" fn rust_vec_push(vec: *mut Vec<u32>, value: u32) {
	let vec: &mut Vec<u32> = unsafe { &mut *vec };
	vec.push(value);
}

#[no_mangle]
pub extern "C" fn rust_vec_make(out: *mut Vec<u32>, count: usize) {
	let vec: Vec<u32> = (0..count as u32).map(|i| i * i).collect();
	unsafe { core::ptr::write(out, vec) };
}

#[no_mangle]
pub extern "C" fn rust_strvec_checksum(vec: *const Vec<String>) -> u64 {
	let vec: &Vec<String> = unsafe { &*vec };
	let mut hash = fnv1a(&[vec.len() as u8]);
	for string in vec.iter() {
		hash = hash.wrapping_add(fnv1a(string.as_bytes()));
	}
	hash
}

#[no_mangle]
pub extern "C" fn rust_strvec_push(vec: *mut Vec<String>, ptr: *const u8, len: usize) {
	let vec: &mut Vec<String> = unsafe { &mut *vec };
	vec.push(String::from(unsafe { borrow(ptr, len) }));
}

#[no_mangle]
pub extern "C" fn rust_strvec_join(vec: *const Vec<String>, out: *mut String) {
	let vec: &Vec<String> = unsafe { &*vec };
	let joined = vec.join(", ");
	unsafe { core::ptr::write(out, joined) };
}

// ---------------------------------------------------------------------------
// Borrowed slices. As arguments the two halves arrive in separate registers,
// and stored in memory they are a { ptr, len } pair.
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn rust_slice_sum(ptr: *const u32, len: usize) -> u64 {
	let slice = unsafe { core::slice::from_raw_parts(ptr, len) };
	slice.iter().map(|x| *x as u64).sum()
}

#[no_mangle]
pub extern "C" fn rust_str_checksum(ptr: *const u8, len: usize) -> u64 {
	fnv1a(unsafe { borrow(ptr, len) }.as_bytes())
}

#[repr(C)]
pub struct FatPointer {
	ptr: usize,
	len: usize,
}

/// Store a &str and a &[u32] borrowed from guest collections, so the host can
/// read them back as GuestRustStr and GuestRustSlice.
#[no_mangle]
pub extern "C" fn rust_borrow_into(string: *const String, vec: *const Vec<u32>, out: *mut FatPointer) {
	let string: &String = unsafe { &*string };
	let vec: &Vec<u32> = unsafe { &*vec };
	unsafe {
		let strref: &str = string.as_str();
		let slice: &[u32] = vec.as_slice();
		let stw: [usize; 2] = core::mem::transmute_copy(&strref);
		let slw: [usize; 2] = core::mem::transmute_copy(&slice);
		core::ptr::write(out.add(0), FatPointer { ptr: stw[0], len: stw[1] });
		core::ptr::write(out.add(1), FatPointer { ptr: slw[0], len: slw[1] });
	}
}

// ---------------------------------------------------------------------------
// The host writing into collections the guest owns, through a system call
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn rust_host_fills_string() -> u64 {
	let mut string = String::new();
	unsafe {
		asm!("ecall", in("a7") SYSCALL_FILL_STRING,
			in("a0") &mut string as *mut String, lateout("a0") _);
	}
	// The guest owns the bytes the host wrote: it can grow them and drop them
	let mut hash = fnv1a(string.as_bytes());
	string.push_str(" and the guest appended to it");
	hash = hash.wrapping_add(fnv1a(string.as_bytes()));
	hash
}

#[no_mangle]
pub extern "C" fn rust_host_fills_vec() -> u64 {
	let mut vec: Vec<u32> = Vec::new();
	unsafe {
		asm!("ecall", in("a7") SYSCALL_FILL_VEC,
			in("a0") &mut vec as *mut Vec<u32>, lateout("a0") _);
	}
	let mut sum: u64 = vec.iter().map(|x| *x as u64).sum();
	vec.push(1000);
	sum += vec.len() as u64;
	sum
}

// ---------------------------------------------------------------------------
// Box: the one Rust smart pointer with a guaranteed layout. A Box the host
// allocated really did come from this program's global allocator, so
// Box::from_raw() on it is sound and not merely convenient - and then the
// ordinary drop at the end of the scope returns it to the host arena.
// ---------------------------------------------------------------------------

/// Take a Box<i64> the host built, and let it drop
#[no_mangle]
pub extern "C" fn rust_box_take_i64(ptr: *mut i64) -> i64 {
	let boxed: Box<i64> = unsafe { Box::from_raw(ptr) };
	*boxed
}

/// Build a Box<i64> and hand ownership to the host
#[no_mangle]
pub extern "C" fn rust_box_make_i64(value: i64) -> *mut i64 {
	Box::into_raw(Box::new(value * 2))
}

/// A struct of the two unsized boxes, so the host can build them in place.
#[repr(C)]
pub struct Boxes {
	pub numbers: Box<[u32]>,
	pub name: Box<str>,
}

/// Take a Box<Boxes> the host built. Dropping it frees three allocations: the
/// struct, the boxed slice and the boxed str, all through the drop glue.
#[no_mangle]
pub extern "C" fn rust_boxes_take(ptr: *mut Boxes) -> u64 {
	let boxes: Box<Boxes> = unsafe { Box::from_raw(ptr) };
	let mut hash = fnv1a(boxes.name.as_bytes());
	for n in boxes.numbers.iter() {
		hash = hash.wrapping_mul(131).wrapping_add(*n as u64);
	}
	hash
}

// ---------------------------------------------------------------------------
// The attribute tree.
//
// Ordinary Rust, with two attributes on it. #[repr(C, u64)] asks for the
// tagged-union layout that the host mirrors - a plain #[repr(Rust)] enum would
// be reordered and niche-packed - and #[repr(C)] does the same for the entry
// struct, which a tuple could not promise.
//
// The entries are kept sorted by key, so a lookup is a binary search. That is
// not a marshalling compromise: it is one allocation for the whole map instead
// of a node per key, it needs no hasher that both sides have to agree on, and
// std::HashMap is one .collect() away for a guest that wants one.
// ---------------------------------------------------------------------------

#[repr(C, u64)]
pub enum Value {
	Nil,
	Int(i64),
	Float(f64),
	Bool(bool),
	Str(String),
	Group(Box<Attributes>),
	List(Box<Vec<Value>>),
}

#[repr(C)]
pub struct Attr {
	pub key: String,
	pub value: Value,
}

#[repr(C)]
pub struct Attributes {
	entries: Vec<Attr>,
}

impl Attributes {
	pub fn new() -> Self {
		Attributes { entries: Vec::new() }
	}

	pub fn get(&self, key: &str) -> Option<&Value> {
		self.entries
			.binary_search_by(|a| a.key.as_str().cmp(key))
			.ok()
			.map(|i| &self.entries[i].value)
	}

	pub fn set(&mut self, key: &str, value: Value) {
		match self.entries.binary_search_by(|a| a.key.as_str().cmp(key)) {
			Ok(i) => self.entries[i].value = value,
			Err(i) => self.entries.insert(i, Attr { key: key.to_string(), value }),
		}
	}

	pub fn len(&self) -> usize {
		self.entries.len()
	}

	pub fn iter(&self) -> impl Iterator<Item = (&str, &Value)> {
		self.entries.iter().map(|a| (a.key.as_str(), &a.value))
	}
}

/// The checksum the host computes over the same tree, with the same operations
/// in the same (sorted) order, so that both sides can prove they are looking at
/// the same thing.
fn checksum_value(value: &Value) -> u64 {
	match value {
		Value::Nil => 1,
		Value::Int(i) => 2u64.wrapping_mul(*i as u64).wrapping_add(0x10),
		Value::Float(f) => 3u64.wrapping_mul(f.to_bits()).wrapping_add(0x20),
		Value::Bool(b) => if *b { 4 } else { 5 },
		Value::Str(s) => fnv1a(s.as_bytes()),
		Value::Group(g) => checksum_attrs(g).wrapping_mul(31),
		Value::List(l) => l
			.iter()
			.fold(7u64, |acc, v| acc.wrapping_mul(131).wrapping_add(checksum_value(v))),
	}
}

fn checksum_attrs(attrs: &Attributes) -> u64 {
	let mut hash = attrs.len() as u64;
	for (key, value) in attrs.iter() {
		hash = hash.wrapping_mul(0x100000001b3);
		hash ^= fnv1a(key.as_bytes());
		hash = hash.wrapping_add(checksum_value(value));
	}
	hash
}

/// The host built a tree on the guest heap and handed over the pointer. The
/// guest takes the whole thing with one from_raw, and drops it on the way out:
/// no rebuilding, and no host-side teardown function either, because the drop
/// glue walks the tree.
#[no_mangle]
pub extern "C" fn rust_attrs_take(ptr: *mut Attributes) -> u64 {
	let attrs: Box<Attributes> = unsafe { Box::from_raw(ptr) };
	checksum_attrs(&attrs)
}

/// The host owns the tree and only lends it out for the duration of the call,
/// which is the shape an engine handing attributes to a script every frame
/// wants: nothing is allocated, nothing is freed.
#[no_mangle]
pub extern "C" fn rust_attrs_borrow(ptr: *const Attributes) -> u64 {
	checksum_attrs(unsafe { &*ptr })
}

/// The guest builds a tree of its own and lends it to the host, which walks it
/// in place. Seven instructions of guest work, whatever the size of the tree.
#[no_mangle]
pub extern "C" fn rust_attrs_build() -> u64 {
	let mut attrs = Attributes::new();
	attrs.set("alpha", Value::Int(-1234));
	attrs.set("beta", Value::Str(String::from("a string the guest allocated")));
	attrs.set("gamma", Value::Float(2.5));
	attrs.set("delta", Value::Bool(true));
	attrs.set("epsilon", Value::Nil);

	let mut group = Attributes::new();
	group.set("nested_int", Value::Int(77));
	group.set("nested_str", Value::Str(String::from("inside the group")));
	attrs.set("zeta", Value::Group(Box::new(group)));

	attrs.set("eta", Value::List(Box::new(vec![
		Value::Int(1), Value::Int(2), Value::Str(String::from("three"))])));

	let checksum = checksum_attrs(&attrs);
	unsafe {
		asm!("ecall", in("a7") SYSCALL_TAKE_ATTRS,
			in("a0") &attrs as *const Attributes, lateout("a0") _);
	}
	checksum
	// The guest still owns it, and drops the whole tree here
}

/// The host fills in a tree the guest owns, through a system call, and the
/// guest then uses it as its own - including growing it.
#[no_mangle]
pub extern "C" fn rust_attrs_host_fills() -> u64 {
	let mut attrs = Attributes::new();
	unsafe {
		asm!("ecall", in("a7") SYSCALL_FILL_ATTRS,
			in("a0") &mut attrs as *mut Attributes, lateout("a0") _);
	}
	let mut hash = checksum_attrs(&attrs);
	// The entries the host allocated are the guest's now: it can insert into
	// the same vector, and it drops all of it at the end of the function
	attrs.set("guest_added", Value::Int(999));
	hash = hash.wrapping_add(checksum_attrs(&attrs));
	hash
}

/// A guest that would rather have a real HashMap gets one for the price of a
/// collect(). The tree is the interchange shape, not a cage.
#[no_mangle]
pub extern "C" fn rust_attrs_into_hashmap(ptr: *mut Attributes) -> u64 {
	let attrs: Box<Attributes> = unsafe { Box::from_raw(ptr) };
	let map: std::collections::HashMap<String, Value> =
		attrs.entries.into_iter().map(|a| (a.key, a.value)).collect();

	let mut total = map.len() as u64;
	for key in ["alpha", "beta", "gamma"] {
		if let Some(value) = map.get(key) {
			total = total.wrapping_add(checksum_value(value));
		}
	}
	total
}

// ---------------------------------------------------------------------------
// Layout probe for the enum and the entry struct, so that a rustc release
// which lays out #[repr(C, u64)] differently fails here instead of silently
// reading garbage everywhere else.
// ---------------------------------------------------------------------------

/// The offset of the payload of a variant, measured against the enum itself
macro_rules! payload_offset {
	($value:expr, $pattern:pat => $field:expr) => {{
		let value = $value;
		let base = &value as *const Value as usize;
		match &value {
			$pattern => ($field as *const _ as usize) - base,
			#[allow(unreachable_patterns)]
			_ => usize::MAX,
		}
	}};
}

/// The discriminant of a variant, read as the first bytes of the enum
fn tag_of(value: &Value) -> usize {
	unsafe { *(value as *const Value as *const u64) as usize }
}

#[no_mangle]
pub extern "C" fn rust_probe_enum_layout(out: *mut usize) {
	let attr = Attr { key: String::from("k"), value: Value::Int(1) };
	let attr_base = &attr as *const Attr as usize;
	let empty = Attributes::new();

	let words: [usize; 21] = [
		core::mem::size_of::<Value>(),
		core::mem::align_of::<Value>(),
		// The discriminants, which must be the declaration order 0..n
		tag_of(&Value::Nil),
		tag_of(&Value::Int(0)),
		tag_of(&Value::Float(0.0)),
		tag_of(&Value::Bool(false)),
		tag_of(&Value::Str(String::new())),
		tag_of(&Value::Group(Box::new(Attributes::new()))),
		tag_of(&Value::List(Box::new(Vec::new()))),
		// The payload offsets, which must all be sizeof(tag)
		payload_offset!(Value::Int(1), Value::Int(x) => x),
		payload_offset!(Value::Float(1.0), Value::Float(x) => x),
		payload_offset!(Value::Bool(true), Value::Bool(x) => x),
		payload_offset!(Value::Str(String::new()), Value::Str(x) => x),
		payload_offset!(Value::Group(Box::new(Attributes::new())), Value::Group(x) => x),
		payload_offset!(Value::List(Box::new(Vec::new())), Value::List(x) => x),
		// The entry struct: a String followed by a Value, in that order
		core::mem::size_of::<Attr>(),
		(&attr.key as *const String as usize) - attr_base,
		(&attr.value as *const Value as usize) - attr_base,
		// Attributes is a newtype over the Vec, so it is the same three words
		core::mem::size_of::<Attributes>(),
		core::mem::size_of::<Box<Attributes>>(),
		empty.len(),
	];
	unsafe { core::ptr::copy_nonoverlapping(words.as_ptr(), out, words.len()) };
}

// ---------------------------------------------------------------------------
// The block memory operations, from the guest side.
//
// Every length here comes in as an argument, which is what keeps LLVM from
// expanding the operation inline: a runtime length has to become a call, and
// the call is what --wrap redirects into the host. Each function returns a
// checksum of the bytes the host produced, so a copy that lands in the wrong
// place cannot pass.
// ---------------------------------------------------------------------------

/// fill() on a byte slice is a memset.
#[no_mangle]
pub extern "C" fn rust_memset_fill(len: usize, byte: u8) -> u64 {
	let mut buffer = vec![0u8; len];
	buffer.fill(byte);
	fnv1a(&buffer)
}

/// copy_from_slice() between two buffers that cannot overlap is a memcpy.
#[no_mangle]
pub extern "C" fn rust_memcpy_roundtrip(len: usize) -> u64 {
	let source: Vec<u8> = (0..len).map(|i| (i % 251) as u8).collect();
	let mut dest = vec![0u8; len];
	dest.copy_from_slice(&source);
	fnv1a(&dest)
}

/// copy_within() overlaps, so it is a memmove. Which way it shifts decides
/// which of the host's two paths runs: forwards when the destination is below
/// the source, backwards when it is above.
#[no_mangle]
pub extern "C" fn rust_memmove_overlap(len: usize, shift: usize, downwards: bool) -> u64 {
	let mut buffer: Vec<u8> = (0..len).map(|i| (i % 251) as u8).collect();
	if downwards {
		buffer.copy_within(shift.., 0);
	} else {
		buffer.copy_within(..len - shift, shift);
	}
	fnv1a(&buffer)
}

/// Comparing two byte slices of a runtime length is a memcmp, and the sign of
/// what the host returns is what Ord turns into Less, Equal or Greater.
#[no_mangle]
pub extern "C" fn rust_memcmp_order(len: usize, index: usize, value: u8) -> i64 {
	let left: Vec<u8> = (0..len).map(|i| (i % 251) as u8).collect();
	let mut right = left.clone();
	if index < len {
		right[index] = value;
	}
	match left.cmp(&right) {
		core::cmp::Ordering::Less => -1,
		core::cmp::Ordering::Equal => 0,
		core::cmp::Ordering::Greater => 1,
	}
}

/// Slice equality, which is the common case and does *not* go to memcmp:
/// LLVM emits the equality-only entry point for it, so this only reaches the
/// host if __memcmpeq (or bcmp on an older toolchain) is wrapped as well.
#[no_mangle]
pub extern "C" fn rust_memcmp_equality(len: usize, index: usize, value: u8) -> u64 {
	let left: Vec<u8> = (0..len).map(|i| (i % 251) as u8).collect();
	let mut right = left.clone();
	if index < len {
		right[index] = value;
	}
	if left == right { 1 } else { 0 }
}

/// A String built out of parts, which is what the collections do all day: the
/// reserve is a realloc into the host arena and the append is a memcpy.
#[no_mangle]
pub extern "C" fn rust_memops_build_string(repeats: usize) -> String {
	let piece = "The guest appends this through the host's memcpy. ";
	let mut text = String::with_capacity(piece.len() * repeats);
	for _ in 0..repeats {
		text.push_str(piece);
	}
	text
}

fn main() {
	println!("Rust guest is ready");
	// Pause here with everything initialized, so that the host can make calls
	unsafe {
		asm!("ecall", in("a7") SYSCALL_PAUSE, lateout("a0") _);
	}
}
)RUST";

// ===========================================================================
// The host half of the attribute tree: the same checksum the guest computes,
// and the same tree built from the host side.
//
// Both walks are written against the mirrors in guest_rust_attributes.hpp, and
// neither of them serializes anything: reading uses string views straight out
// of guest memory, and writing allocates the entries in their final shape on
// the guest heap, which is the host's own arena.
// ===========================================================================

static uint64_t host_checksum_attrs(const Machine<RISCV64>& machine, const RustAttrs& attrs);

static uint64_t host_checksum_value(const Machine<RISCV64>& machine, const RustValue& value)
{
	uint64_t result = 0;
	const bool ok = value.visit([&] (const auto& alt) {
		using T = std::decay_t<decltype(alt)>;
		if constexpr (std::is_same_v<T, std::monostate>) {
			result = 1;
		} else if constexpr (std::is_same_v<T, int64_t>) {
			result = 2 * uint64_t(alt) + 0x10;
		} else if constexpr (std::is_same_v<T, double>) {
			uint64_t bits;
			std::memcpy(&bits, &alt, sizeof(bits));
			result = 3 * bits + 0x20;
		} else if constexpr (std::is_same_v<T, bool>) {
			result = alt ? 4 : 5;
		} else if constexpr (std::is_same_v<T, RustString>) {
			result = fnv1a(alt.to_view(machine));
		} else if constexpr (std::is_same_v<T, RustGroup>) {
			result = host_checksum_attrs(machine, alt.get(machine)) * 31;
		} else if constexpr (std::is_same_v<T, RustList>) {
			const auto& list = alt.get(machine);
			result = 7;
			for (std::size_t i = 0; i < list.size(); i++)
				result = result * 131 + host_checksum_value(machine, list.at(machine, i));
		} else {
			static_assert(sizeof(T) == 0, "Unhandled attribute variant");
		}
	});
	REQUIRE(ok);
	return result;
}

static uint64_t host_checksum_attrs(const Machine<RISCV64>& machine, const RustAttrs& attrs)
{
	uint64_t hash = attrs.size();
	attrs.for_each(machine, [&] (std::string_view key, const RustValue& value) {
		hash = hash * 0x100000001b3ull;
		hash ^= fnv1a(key);
		hash += host_checksum_value(machine, value);
	});
	return hash;
}

/// @brief Build the same tree the guest's rust_attrs_build() builds, directly
/// on the guest heap. Nothing is copied afterwards: this is the final shape.
static void host_build_attrs(Machine<RISCV64>& machine, RustAttrs& attrs)
{
	attrs.insert_or_assign(machine, "alpha", int64_t(-1234));
	attrs.insert_or_assign(machine, "beta", GUEST_BETA);
	attrs.insert_or_assign(machine, "gamma", 2.5);
	attrs.insert_or_assign(machine, "delta", true);
	attrs.emplace<std::monostate>(machine, "epsilon");

	// A nested group: one Box holding an Attributes of its own
	auto& group = attrs.emplace<RustGroup>(machine, "zeta").get(machine);
	group.insert_or_assign(machine, "nested_int", int64_t(77));
	group.insert_or_assign(machine, "nested_str", GUEST_NESTED);

	// A list: one Box holding a Vec of values
	auto& list = attrs.emplace<RustList>(machine, "eta").get(machine);
	list.resize(machine, 3);
	list.at(machine, 0).set(machine, int64_t(1));
	list.at(machine, 1).set(machine, int64_t(2));
	list.at(machine, 2).set(machine, std::string_view("three"));
}

/// @brief The guest program, built once for the whole test binary.
///
/// --wrap makes the linker rewrite every reference to memcpy, memset, memmove
/// and memcmp - Rust's own and the ones inside glibc - to the __wrap_* stubs
/// at the top of the guest, which turn them into the host's memory syscalls.
static const std::vector<uint8_t>& rust_guest_binary()
{
	static const std::string RUST_ARGS = "-C opt-level=2"
		" -C link-arg=-Wl,--wrap=memcpy"
		" -C link-arg=-Wl,--wrap=memset"
		" -C link-arg=-Wl,--wrap=memmove"
		" -C link-arg=-Wl,--wrap=memcmp"
		// Slice equality goes to the equality-only entry point, not to memcmp
		" -C link-arg=-Wl,--wrap=__memcmpeq"
		" -C link-arg=-Wl,--wrap=bcmp";
	static const std::vector<uint8_t> binary =
		build_rust_and_load(RUST_GUEST_PROGRAM, RUST_ARGS);
	return binary;
}

/// @brief The same guest binary, with one machine pinned for the lifetime of
/// the test binary.
///
/// libriscv shares the decoded execute segment between machines built from the
/// same binary, but the shared entry is dropped again as soon as the last
/// machine using it goes away. Every test below builds its own machine and lets
/// it go, so without one machine held open the guest is decoded once per test
/// case - and under a JIT backend that means compiling a statically linked Rust
/// program from scratch about forty times, which is most of the test run.
///
/// The pinned machine is never simulated and never destroyed; it only has to
/// exist. It is reached through a static pointer, so it stays a root as far as
/// the leak sanitizer is concerned, and its destructor never runs at exit.
static const std::vector<uint8_t>& pinned_rust_guest_binary()
{
	const auto& binary = rust_guest_binary();
	// Default options here, matching the machines the tests build: the shared
	// segment is keyed on the arena size as well as the code itself.
	static Machine<RISCV64>* pinned = new Machine<RISCV64>(binary);
	(void) pinned;
	return binary;
}

// ---------------------------------------------------------------------------
// Counting the block memory operations.
//
// libriscv installs plain function pointers, so counting the calls means
// keeping the originals and chaining onto them. Without this a test could only
// see that the bytes came out right, which a guest-local memcpy would also
// manage: the counters are what prove the work happened on the host.
// ---------------------------------------------------------------------------

enum MemoryOp { MEMCPY = 0, MEMSET = 1, MEMMOVE = 2, MEMCMP = 3, MEMOP_COUNT = 4 };

static std::array<uint64_t, MEMOP_COUNT> g_memop_counts {};
static std::array<Machine<RISCV64>::syscall_t, MEMOP_COUNT> g_memop_originals {};

template <size_t Index>
static void counting_memory_syscall(Machine<RISCV64>& machine)
{
	g_memop_counts[Index] ++;
	g_memop_originals[Index](machine);
}

/// @brief Install the native memory syscalls with a counter chained onto each.
/// Installing twice would chain the counters onto themselves, so it is done
/// exactly once for the whole test binary.
static void install_counted_memory_syscalls()
{
	static bool installed = false;
	if (installed)
		return;
	installed = true;

	Machine<RISCV64>::setup_native_memory(MEMORY_SYSCALLS_BASE);
	for (int op = 0; op < MEMOP_COUNT; op++)
		g_memop_originals[op] = Machine<RISCV64>::syscall_handlers.at(MEMORY_SYSCALLS_BASE + op);

	Machine<RISCV64>::install_syscall_handlers({
		{MEMORY_SYSCALLS_BASE + MEMCPY,  counting_memory_syscall<MEMCPY>},
		{MEMORY_SYSCALLS_BASE + MEMSET,  counting_memory_syscall<MEMSET>},
		{MEMORY_SYSCALLS_BASE + MEMMOVE, counting_memory_syscall<MEMMOVE>},
		{MEMORY_SYSCALLS_BASE + MEMCMP,  counting_memory_syscall<MEMCMP>},
	});
}

/// @brief A baseline of the counters, so a test can say how many of each
/// operation its own guest call made.
struct MemoryOpCounter {
	MemoryOpCounter() : m_baseline(g_memop_counts) {}
	uint64_t operator[](int op) const { return g_memop_counts[op] - m_baseline[op]; }
	void reset() { m_baseline = g_memop_counts; }
private:
	std::array<uint64_t, MEMOP_COUNT> m_baseline;
};

/// @brief A block of raw bytes in the guest arena. A vmcall string argument is
/// zero-terminated, which is not what a Rust &str is, so the tests hand the
/// guest an address and a length instead.
struct GuestBytes {
	GuestBytes(Machine<RISCV64>& machine, std::string_view bytes)
		: m_machine(machine), m_len(bytes.size())
	{
		m_addr = machine.arena().malloc(bytes.size() + 1);
		REQUIRE(m_addr != 0);
		machine.copy_to_guest(m_addr, bytes.data(), bytes.size());
	}
	~GuestBytes() { m_machine.arena().free(m_addr); }

	uint64_t address() const noexcept { return m_addr; }
	uint64_t size() const noexcept { return m_len; }

	Machine<RISCV64>& m_machine;
	uint64_t m_addr;
	uint64_t m_len;
};

/// @brief A booted Rust guest, paused inside main() with its runtime fully
/// initialized and its heap taken over by the host arena.
struct RustGuest {
	Machine<RISCV64> machine;

	RustGuest() : machine(pinned_rust_guest_binary())
	{
		const auto heap = machine.memory.mmap_allocate(HEAP_SIZE);
		machine.setup_native_heap(HEAP_SYSCALLS_BASE, heap, HEAP_SIZE);
		// memcpy, memset, memmove and memcmp, done natively by the host. The
		// guest is linked with --wrap, so every one of them arrives here
		// instead of being emulated one instruction at a time.
		install_counted_memory_syscalls();
		machine.setup_linux_syscalls();
		// Rust's standard library locks with futexes even single-threaded
		machine.setup_posix_threads();
		machine.setup_linux({"rust_guest"}, {"LC_ALL=C", "USER=root"});

		install_syscalls();

		machine.simulate(MAX_INSTRUCTIONS);
		// main() does not return: it stops the machine from inside the pause
		// system call, with everything initialized and ready to be called into
		REQUIRE(!machine.instruction_limit_reached());
		REQUIRE(machine.stopped());
	}

	Machine<RISCV64>* operator->() { return &machine; }

	/// @brief Run through every collection once, so that the lazy allocations
	/// the runtime makes are not counted against the tests below
	void warmup() { machine.vmcall("rust_warmup"); }

	/// @brief Live guest allocations, to prove that a test leaks nothing
	unsigned live_allocations() const {
		return machine.arena().allocation_counter() - machine.arena().deallocation_counter();
	}

private:
	static void install_syscalls()
	{
		// The system call handlers are static, so installing them once is
		// enough, but doing it again is harmless
		Machine<RISCV64>::install_syscall_handler(SYSCALL_PAUSE,
		[] (Machine<RISCV64>& machine) {
			machine.stop();
		});

		// The guest hands the host a String it owns, and the host fills it in
		Machine<RISCV64>::install_syscall_handler(SYSCALL_FILL_STRING,
		[] (Machine<RISCV64>& machine) {
			const auto self = machine.sysarg(0);
			auto& str = *machine.memory.memarray<RustString>(self, 1);
			str.set_string(machine, HOST_TEXT);
			machine.set_result(0);
		});

		// The same for a Vec<u32>
		Machine<RISCV64>::install_syscall_handler(SYSCALL_FILL_VEC,
		[] (Machine<RISCV64>& machine) {
			const auto self = machine.sysarg(0);
			auto& vec = *machine.memory.memarray<RustVec<uint32_t>>(self, 1);
			for (uint32_t i = 1; i <= 10; i++)
				vec.push_back(machine, i);
			machine.set_result(0);
		});

		// The guest lends the host a &str: the address and the length are two
		// separate registers, which is how Rust passes one
		Machine<RISCV64>::install_syscall_handler(SYSCALL_TAKE_STR,
		[] (Machine<RISCV64>& machine) {
			const RustStr text(machine.sysarg(0), machine.sysarg(1));
			g_lent_str = text.to_string(machine);
			machine.set_result(0);
		});

		// The guest lends the host a whole Vec<String>, by address. The host
		// only reads it: the guest still owns it and drops it afterwards
		Machine<RISCV64>::install_syscall_handler(SYSCALL_TAKE_VEC,
		[] (Machine<RISCV64>& machine) {
			const auto self = machine.sysarg(0);
			const auto& vec = *machine.memory.memarray<const RustVec<RustString>>(self, 1);
			g_lent_vec = vec.to_string_vector(machine);
			machine.set_result(0);
		});

		// The guest lends the host its whole attribute tree, by address. The
		// host walks it in place: no marshalling, and no guest work beyond
		// handing over the address of its own three words.
		Machine<RISCV64>::install_syscall_handler(SYSCALL_TAKE_ATTRS,
		[] (Machine<RISCV64>& machine) {
			const auto self = machine.sysarg(0);
			const auto& attrs = *machine.memory.memarray<const RustAttrs>(self, 1);
			// A script is adversarial input, so the shape is checked first
			attrs.validate(machine);
			REQUIRE(attrs.is_sorted(machine));

			g_lent_attrs_size = attrs.size();
			g_lent_attrs_checksum = host_checksum_attrs(machine, attrs);

			// Reading individual keys goes through the same guest memory
			REQUIRE(attrs.at(machine, "alpha").get<int64_t>() == -1234);
			REQUIRE(attrs.at(machine, "beta").get<RustString>().to_string(machine) == GUEST_BETA);
			REQUIRE(attrs.at(machine, "gamma").get<double>() == 2.5);
			REQUIRE(attrs.at(machine, "delta").get<bool>() == true);
			REQUIRE(attrs.at(machine, "epsilon").holds_alternative<std::monostate>());
			REQUIRE(attrs.find(machine, "missing") == nullptr);

			const auto& group = attrs.at(machine, "zeta").get<RustGroup>().get(machine);
			REQUIRE(group.size() == 2);
			REQUIRE(group.at(machine, "nested_int").get<int64_t>() == 77);
			REQUIRE(group.at(machine, "nested_str").get<RustString>().to_string(machine) == GUEST_NESTED);

			const auto& list = attrs.at(machine, "eta").get<RustList>().get(machine);
			REQUIRE(list.size() == 3);
			REQUIRE(list.at(machine, 2).get<RustString>().to_string(machine) == "three");

			machine.set_result(0);
		});

		// The guest hands the host an attribute tree it owns, and the host
		// builds the whole thing into it. Every entry is allocated in its
		// final position on the guest heap, so the guest has nothing to do.
		Machine<RISCV64>::install_syscall_handler(SYSCALL_FILL_ATTRS,
		[] (Machine<RISCV64>& machine) {
			const auto self = machine.sysarg(0);
			auto& attrs = *machine.memory.memarray<RustAttrs>(self, 1);
			host_build_attrs(machine, attrs);
			machine.set_result(0);
		});
	}
};

// ===========================================================================
// Worked examples
//
// Five short tests, one per way of moving a Rust value across the boundary.
// Read these first; everything after them is verification, not instruction.
// ===========================================================================

// The plainest case: the host builds a Rust String in its own arena and calls
// the guest with it. A ScopedRustString owns the three words *and* the bytes
// they point at, and passing it to vmcall() passes its guest address, which is
// what a `&String` or a `*const String` parameter expects.
TEST_CASE("Example: the host builds a String and a Vec for the guest", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	static const std::string TEXT = "Hello from the host";

	// One warm-up call first: the Rust runtime allocates lazily, and the
	// allocation balance at the end of the test would move underneath us
	guest.warmup();
	const unsigned allocs_before = guest.live_allocations();

	{
		// fn rust_string_checksum(string: *const String) -> u64
		ScopedRustString text(guest.machine, TEXT);
		REQUIRE(guest->vmcall("rust_string_checksum", text) == fnv1a(TEXT));

		// fn rust_vec_sum(vec: *const Vec<u32>) -> u64
		ScopedRustVec<uint32_t> vec(guest.machine, std::vector<uint32_t>{ 1, 2, 3, 4 });
		REQUIRE(guest->vmcall("rust_vec_sum", vec) == 10);

		// A borrowed &str is not the two-word struct: the RISC-V ABI hands the
		// guest its address and its length in two separate registers, so it is
		// two vmcall arguments
		const RustStr borrowed = text->as_str();
		REQUIRE(guest->vmcall("rust_str_checksum", borrowed.address(), borrowed.size())
			== fnv1a(TEXT));
	}
	// Both scoped objects freed themselves, and their guest allocations
	REQUIRE(guest.live_allocations() == allocs_before);
}

// A String or a Vec is too large to come back in a register, so a Rust function
// that returns one does not return it in a0: the caller allocates room for it
// and passes the address as a hidden first argument. On the host that is the
// same thing as handing vmcall() a scoped object first - the guest writes its
// return value straight into it.
TEST_CASE("Example: a Rust function that returns a String or a Vec", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	guest.warmup();
	const unsigned allocs_before = guest.live_allocations();

	{
		// fn rust_make_greeting() -> String
		ScopedRustString greeting(guest.machine);
		guest->vmcall("rust_make_greeting", greeting);
		REQUIRE(greeting->to_string(guest.machine)
			== "A String returned by value from the guest");

		// fn rust_make_squares(count: usize) -> Vec<u32>
		// The hidden pointer takes the first register, so the real argument
		// `count` is the *second* vmcall argument
		ScopedRustVec<uint32_t> squares(guest.machine);
		guest->vmcall("rust_make_squares", squares, 5u);
		REQUIRE(squares->to_vector(guest.machine)
			== std::vector<uint32_t>{ 0, 1, 4, 9, 16 });
	}
	// Allocated by the guest, freed by the host, in the one shared arena
	REQUIRE(guest.live_allocations() == allocs_before);
}

// The other direction: the guest decides to hand something to the host, in a
// system call. Neither of these transfers ownership - the host reads the bytes
// where they lie, and the guest drops them as usual on the way out.
TEST_CASE("Example: the guest lends data to the host", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	guest.warmup();
	const unsigned allocs_before = guest.live_allocations();

	// A &str, as an address and a length in two registers
	g_lent_str.clear();
	guest->vmcall("rust_lends_str");
	REQUIRE(g_lent_str == LENT_TEXT);

	// A whole Vec<String>, as the address of its three words
	g_lent_vec.clear();
	guest->vmcall("rust_lends_vec");
	REQUIRE(g_lent_vec == LENT_NAMES);

	REQUIRE(guest.live_allocations() == allocs_before);
}

// Collections nest. An element of a Vec<Vec<u32>> is itself the three words of
// a Vec, so the host builds the inner vectors exactly like any other element
// and pushes them in. Freeing the outer one frees them all.
TEST_CASE("Example: nested Rust collections", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	guest.warmup();
	const unsigned allocs_before = guest.live_allocations();

	{
		ScopedRustVec<RustVec<uint32_t>> rows(guest.machine);
		for (uint32_t r = 0; r < 3; r++) {
			// A shallow push: the row hands its allocation over to the array
			RustVec<uint32_t> row(guest.machine, std::vector<uint32_t>{ r, r + 1, r + 2 });
			rows->push_back(guest.machine, row);
		}
		REQUIRE(rows->size() == 3);
		REQUIRE(rows->at(guest.machine, 2).to_vector(guest.machine)
			== std::vector<uint32_t>{ 2, 3, 4 });

		// fn rust_nested_sum(vec: *const Vec<Vec<u32>>) -> u64
		REQUIRE(guest->vmcall("rust_nested_sum", rows) == (0+1+2) + (1+2+3) + (2+3+4));
	}
	// The outer vector freed the three inner ones along with itself
	REQUIRE(guest.live_allocations() == allocs_before);
}

// Ownership crosses in both directions, because both sides allocate out of the
// same arena. Here the host hands over a raw block it filled in itself, and
// afterwards the guest grows and frees it through its own global allocator.
// (For the reverse, see "Rust String ownership moves to the guest" below.)
TEST_CASE("Example: the guest takes ownership of a host buffer", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	guest.warmup();
	const unsigned allocs_before = guest.live_allocations();

	{
		ScopedRustVec<uint32_t> vec(guest.machine);

		// A plain arena block, filled in by the host
		const auto buffer = guest->arena().malloc(4 * sizeof(uint32_t));
		REQUIRE(buffer != 0);
		auto* values = guest->memory.memarray<uint32_t>(buffer, 4);
		for (uint32_t i = 0; i < 4; i++)
			values[i] = 10 * (i + 1);

		// The vector adopts it, unchanged: no copy, and both the length and
		// the capacity become the element count that was handed over
		vec->assume_ownership(guest.machine, buffer, 4);
		REQUIRE(vec->data() == buffer);
		REQUIRE(vec->size() == 4);
		REQUIRE(vec->capacity() == 4);
		REQUIRE(guest->vmcall("rust_vec_sum", vec) == 100);

		// Pushing past the capacity makes the guest reallocate it through the
		// global allocator, which is this same arena
		guest->vmcall("rust_vec_push", vec, 50u);
		REQUIRE(vec->size() == 5);
		REQUIRE(vec->capacity() >= 5);
		REQUIRE(guest->vmcall("rust_vec_sum", vec) == 150);
	}
	REQUIRE(guest.live_allocations() == allocs_before);
}

// ===========================================================================
// Verification of the host mirrors
// ===========================================================================

// The layout of a Rust collection is not stabilized by the language: the
// compiler is free to reorder the fields, and the host mirrors were written
// from what it emits today. This test asks the guest itself, so that a future
// Rust release that moves a field fails here instead of silently reading
// garbage everywhere else.
TEST_CASE("Rust collection layout matches the host mirrors", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;

	// The length of the string that rust_probe_layout() builds
	static constexpr uint64_t PROBE_STRING_LEN =
		sizeof("A string that is definitely heap allocated") - 1;

	// 26 words: 3 raw words + ptr/len/capacity for the Vec and the String,
	// 2 raw words + ptr/len for the slice and the &str, and the raw words of
	// an empty String and an empty Vec
	const auto addr = guest->arena().malloc(26 * sizeof(uint64_t));
	guest->vmcall("rust_probe_layout", addr);
	const auto* w = guest->memory.memarray<const uint64_t>(addr, 26);

	// Vec<u32> = { capacity, ptr, len }
	REQUIRE(w[0] == w[5]);  // capacity comes first, ahead of the pointer
	REQUIRE(w[1] == w[3]);  // then the pointer
	REQUIRE(w[2] == w[4]);  // then the length
	REQUIRE(w[4] == 3);
	REQUIRE(w[5] >= 3);

	// String = Vec<u8>, ie. the same three words
	REQUIRE(w[6] == w[11]);
	REQUIRE(w[7] == w[9]);
	REQUIRE(w[8] == w[10]);
	REQUIRE(w[10] == PROBE_STRING_LEN);

	// &[u32] and &str = { ptr, len }
	REQUIRE(w[12] == w[14]);
	REQUIRE(w[13] == w[15]);
	REQUIRE(w[15] == 3);
	REQUIRE(w[16] == w[18]);
	REQUIRE(w[17] == w[19]);
	REQUIRE(w[19] == PROBE_STRING_LEN);

	// Reading the raw words through the host mirrors has to agree with what
	// the Rust API reported for the very same collection
	RustVec<uint32_t> vec;
	vec.cap = w[0]; vec.ptr = w[1]; vec.len = w[2];
	REQUIRE(vec.capacity() == w[5]);
	REQUIRE(vec.data() == w[3]);
	REQUIRE(vec.size() == w[4]);

	RustString str;
	str.cap = w[6]; str.ptr = w[7]; str.len = w[8];
	REQUIRE(str.capacity() == w[11]);
	REQUIRE(str.data() == w[9]);
	REQUIRE(str.size() == w[10]);

	const RustStr strref(w[16], w[17]);
	REQUIRE(strref.address() == w[18]);
	REQUIRE(strref.size() == w[19]);

	// An empty collection has a capacity of zero and a dangling, aligned,
	// never null pointer (Rust holds a NonNull<T>)
	REQUIRE(w[20] == 0);
	REQUIRE(w[21] == RustString::DANGLING);
	REQUIRE(w[22] == 0);
	REQUIRE(w[23] == 0);
	REQUIRE(w[24] == RustVec<uint32_t>::DANGLING);
	REQUIRE(w[25] == 0);

	guest->arena().free(addr);
}

// reserve() claims to grow exactly the way Rust's RawVec does. Both sides push
// one element at a time and must end up with the same capacity, or a collection
// the host builds will reallocate the moment the guest touches it.
TEST_CASE("Rust RawVec growth matches the host", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;

	for (const size_t pushes : {0u, 1u, 2u, 3u, 4u, 5u, 8u, 9u, 16u, 17u, 33u, 100u})
	{
		{
			ScopedRustVec<uint32_t> vec(guest.machine);
			for (size_t i = 0; i < pushes; i++)
				vec->push_back(guest.machine, uint32_t(i));

			const uint64_t guest_capacity =
				guest->vmcall("rust_vec_capacity_after", pushes);
			INFO("Vec<u32> after " << pushes << " pushes");
			REQUIRE(vec->size() == pushes);
			REQUIRE(vec->capacity() == guest_capacity);
		}
		{
			ScopedRustString str(guest.machine);
			for (size_t i = 0; i < pushes; i++)
				str->append(guest.machine, "x");

			const uint64_t guest_capacity =
				guest->vmcall("rust_string_capacity_after", pushes);
			INFO("String after " << pushes << " pushes");
			REQUIRE(str->size() == pushes);
			REQUIRE(str->capacity() == guest_capacity);
		}
	}

	// The smallest allocation Rust makes: 8 bytes, 4 elements of 4 bytes
	REQUIRE(RustString::MIN_CAPACITY == 8);
	REQUIRE(RustVec<uint32_t>::MIN_CAPACITY == 4);
}

TEST_CASE("VM calls with a Rust String", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	static const std::string TEXT = "Hello from the host, Rust World!";
	static const std::string APPENDED = " ...and some more text on the end";

	// One warm-up call, as the guest runtime allocates lazily
	guest.warmup();
	const unsigned allocs_before = guest.live_allocations();

	for (int i = 0; i < 10; i++)
	{
		ScopedRustString str(guest.machine);
		REQUIRE(str->empty());
		REQUIRE(str->capacity() == 0);
		REQUIRE(str->data() == RustString::DANGLING);

		str = TEXT;
		REQUIRE(str->size() == TEXT.size());
		REQUIRE(str->to_view(guest.machine) == TEXT);
		REQUIRE(str->to_string(guest.machine) == TEXT);

		// The guest reads the string the host built
		REQUIRE(guest->vmcall("rust_string_len", str) == TEXT.size());
		REQUIRE(guest->vmcall("rust_string_checksum", str) == fnv1a(TEXT));

		// The guest appends to it, which reallocates it in the arena
		GuestBytes more(guest.machine, APPENDED);
		guest->vmcall("rust_string_append", str, more.address(), more.size());
		REQUIRE(str->to_view(guest.machine) == TEXT + APPENDED);
		REQUIRE(str->capacity() >= TEXT.size() + APPENDED.size());

		// A string passed by value ends up on the guest stack, and the guest
		// receives a pointer to it, exactly like a &String argument
		RustString onstack(guest.machine, TEXT);
		REQUIRE(guest->vmcall("rust_string_checksum", onstack) == fnv1a(TEXT));
		onstack.free(guest.machine);
	}

	REQUIRE(guest.live_allocations() == allocs_before);
}

TEST_CASE("A Rust String the guest builds and the host frees", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	static const std::string TEXT = "A string that starts life on the host side";

	guest.warmup();
	const unsigned allocs_before = guest.live_allocations();

	for (int i = 0; i < 10; i++)
	{
		// The guest writes a String of its own into an address the host picked
		ScopedRustString str(guest.machine);
		GuestBytes text(guest.machine, TEXT);
		guest->vmcall("rust_string_make", str, text.address(), text.size());

		REQUIRE(str->to_view(guest.machine) == TEXT + " (built by the guest)");
		REQUIRE(str->capacity() >= str->size());
		// Freed by the ScopedArenaObject, with the guest's own allocation

		// And the other way around: the guest drops a host-built string
		ScopedRustString hostside(guest.machine, TEXT);
		REQUIRE(hostside->capacity() != 0);
		guest->vmcall("rust_string_drop", hostside);
		REQUIRE(hostside->empty());
		REQUIRE(hostside->capacity() == 0);
		REQUIRE(hostside->data() == RustString::DANGLING);
	}

	REQUIRE(guest.live_allocations() == allocs_before);
}

// The guest can take ownership of memory the host allocated, and keep it long
// after the call returns. A moved-from string is left empty, so freeing it on
// the host side releases nothing.
TEST_CASE("Rust String ownership moves to the guest", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	static const std::string TEXT = "This string is heap-allocated and becomes guest-owned!";

	guest.warmup();
	const unsigned allocs_before = guest.live_allocations();

	{
		ScopedRustString str(guest.machine, TEXT);
		REQUIRE(str->to_view(guest.machine) == TEXT);

		guest->vmcall("rust_take_string", str);

		// The guest moved out of it, leaving an empty String behind
		REQUIRE(str->empty());
		REQUIRE(str->capacity() == 0);
		REQUIRE(str->data() == RustString::DANGLING);

		// ... and it still owns the bytes the host allocated
		REQUIRE(guest->vmcall("rust_stored_checksum") == fnv1a(TEXT));
	}
	// The scoped string is gone, and the guest's copy is still valid
	REQUIRE(guest->vmcall("rust_stored_checksum") == fnv1a(TEXT));
	REQUIRE(guest.live_allocations() == allocs_before + 1);

	// The guest drops it, which returns the host's allocation to the arena
	guest->vmcall("rust_release_stored");
	REQUIRE(guest->vmcall("rust_stored_checksum") == 0);
	REQUIRE(guest.live_allocations() == allocs_before);
}

TEST_CASE("VM calls with a Rust Vec", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;

	guest.warmup();
	const unsigned allocs_before = guest.live_allocations();

	for (int i = 0; i < 10; i++)
	{
		ScopedRustVec<uint32_t> vec(guest.machine, std::vector<uint32_t>{ 1, 2, 3, 4, 5 });
		REQUIRE(vec->size() == 5);
		REQUIRE(vec->to_vector(guest.machine) == std::vector<uint32_t>{ 1, 2, 3, 4, 5 });
		REQUIRE(guest->vmcall("rust_vec_sum", vec) == 15);

		// The guest pushes, which reallocates the vector in the arena
		for (uint32_t v = 6; v <= 20; v++)
			guest->vmcall("rust_vec_push", vec, v);
		REQUIRE(vec->size() == 20);
		REQUIRE(guest->vmcall("rust_vec_sum", vec) == 20 * 21 / 2);
		REQUIRE(vec->at(guest.machine, 19) == 20);

		// The host reads it back element by element
		uint64_t sum = 0;
		for (const uint32_t value : vec->to_vector(guest.machine))
			sum += value;
		REQUIRE(sum == 20 * 21 / 2);

		// Shrinking keeps the allocation, like Rust's truncate()
		const auto capacity = vec->capacity();
		vec->resize(guest.machine, 3);
		REQUIRE(vec->size() == 3);
		REQUIRE(vec->capacity() == capacity);
		REQUIRE(guest->vmcall("rust_vec_sum", vec) == 6);

		vec->clear(guest.machine);
		REQUIRE(vec->empty());
		REQUIRE(guest->vmcall("rust_vec_sum", vec) == 0);

		// A vector the guest builds at an address the host picked
		ScopedRustVec<uint32_t> made(guest.machine);
		guest->vmcall("rust_vec_make", made, 8u);
		REQUIRE(made->size() == 8);
		for (size_t k = 0; k < 8; k++)
			REQUIRE(made->at(guest.machine, k) == k * k);
	}

	REQUIRE(guest.live_allocations() == allocs_before);
}

TEST_CASE("VM calls with a Rust Vec<String>", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	static const std::vector<std::string> NAMES {
		"one", "two", "three", "a name long enough to need its own allocation"
	};

	guest.warmup();
	const unsigned allocs_before = guest.live_allocations();

	for (int i = 0; i < 10; i++)
	{
		ScopedRustVec<RustString> names(guest.machine, NAMES);
		REQUIRE(names->size() == NAMES.size());
		REQUIRE(names->to_string_vector(guest.machine) == NAMES);

		auto expected = [] (const std::vector<std::string>& strings) {
			const uint8_t count = uint8_t(strings.size());
			uint64_t hash = fnv1a(std::string_view((const char *)&count, 1));
			for (const auto& string : strings)
				hash += fnv1a(string);
			return hash;
		};
		REQUIRE(guest->vmcall("rust_strvec_checksum", names) == expected(NAMES));

		// The guest pushes a String of its own into the host's vector
		static const std::string EXTRA = "an element the guest pushed on the end";
		GuestBytes extra(guest.machine, EXTRA);
		guest->vmcall("rust_strvec_push", names, extra.address(), extra.size());

		auto all = NAMES;
		all.push_back(EXTRA);
		REQUIRE(names->size() == all.size());
		REQUIRE(names->to_string_vector(guest.machine) == all);
		REQUIRE(guest->vmcall("rust_strvec_checksum", names) == expected(all));

		// And joins them into a String at an address the host picked
		ScopedRustString joined(guest.machine);
		guest->vmcall("rust_strvec_join", names, joined);

		std::string host_joined;
		for (size_t k = 0; k < all.size(); k++)
			host_joined += (k ? ", " : "") + all[k];
		REQUIRE(joined->to_view(guest.machine) == host_joined);
	}

	REQUIRE(guest.live_allocations() == allocs_before);
}

// A &str or a &[T] argument is not the two-word struct: the RISC-V ABI hands
// the guest the address and the length in separate registers.
TEST_CASE("Borrowed Rust &str and &[T]", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	static const std::string TEXT = "Borrowed by the guest, owned by the host";

	ScopedRustString str(guest.machine, TEXT);
	ScopedRustVec<uint32_t> vec(guest.machine, std::vector<uint32_t>{ 10, 20, 30, 40 });

	// As arguments: two registers, not a pointer to a fat pointer
	const RustStr borrowed = str->as_str();
	REQUIRE(borrowed.size() == TEXT.size());
	REQUIRE(borrowed.to_view(guest.machine) == TEXT);
	REQUIRE(guest->vmcall("rust_str_checksum", borrowed.address(), borrowed.size())
		== fnv1a(TEXT));

	const RustSlice<uint32_t> slice = vec->as_slice();
	REQUIRE(slice.size() == 4);
	REQUIRE(slice.as_array(guest.machine)[2] == 30);
	REQUIRE(guest->vmcall("rust_slice_sum", slice.address(), slice.size()) == 100);

	// Stored in memory they are { ptr, len }, which is what the guest writes
	// out here, and what the host mirrors read back
	const auto addr = guest->arena().malloc(2 * sizeof(RustStr));
	guest->vmcall("rust_borrow_into", str, vec, addr);

	const auto& guest_str = *guest->memory.memarray<const RustStr>(addr, 1);
	REQUIRE(guest_str.address() == str->data());
	REQUIRE(guest_str.size() == TEXT.size());
	REQUIRE(guest_str.to_view(guest.machine) == TEXT);

	const auto& guest_slice =
		*guest->memory.memarray<const RustSlice<uint32_t>>(addr + sizeof(RustStr), 1);
	REQUIRE(guest_slice.address() == vec->data());
	REQUIRE(guest_slice.size() == 4);
	REQUIRE(guest_slice.as_array(guest.machine)[3] == 40);

	// An empty borrow is a dangling pointer and a zero length
	const RustStr empty;
	REQUIRE(empty.empty());
	REQUIRE(empty.address() == RustString::DANGLING);
	REQUIRE(empty.to_view(guest.machine).empty());

	guest->arena().free(addr);
}

// The other direction of the same trick: the guest owns the collection, and
// hands the host the address of it in a system call.
TEST_CASE("Host fills in guest-owned Rust collections", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;

	guest.warmup();
	const unsigned allocs_before = guest.live_allocations();

	for (int i = 0; i < 10; i++)
	{
		const uint64_t expected_string =
			fnv1a(HOST_TEXT) + fnv1a(HOST_TEXT + GUEST_SUFFIX);
		REQUIRE(guest->vmcall("rust_host_fills_string") == expected_string);

		// 1..10 summed, plus the length after the guest pushed one more
		REQUIRE(guest->vmcall("rust_host_fills_vec") == 55 + 11);
	}

	// Every collection was dropped by the guest, releasing the host's memory
	REQUIRE(guest.live_allocations() == allocs_before);
}

// Rust is allowed to assume that the bytes of a String are valid UTF-8, and the
// host writes into those bytes unchecked. is_valid_utf8() is the check to do
// beforehand, so it has to agree with Rust's own on every edge case.
TEST_CASE("Rust UTF-8 validation matches the guest", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;

	static const std::vector<std::string> CASES {
		"",                          // empty
		"Hello, World!",             // plain ASCII
		std::string("a\0b", 3),      // an embedded zero is just a character
		"\x7F",                      // last single byte
		"\xC2\x80",                  // U+0080, the first two-byte sequence
		"\xDF\xBF",                  // U+07FF, the last one
		"\xE0\xA0\x80",              // U+0800, the first three-byte sequence
		"\xEF\xBF\xBF",              // U+FFFF
		"\xF0\x90\x80\x80",          // U+10000, the first four-byte sequence
		"\xF4\x8F\xBF\xBF",          // U+10FFFF, the last code point
		"\xF0\x9F\x98\x80 smile",    // an emoji
		"\x80",                      // a lone continuation byte
		"\xC0\xAF",                  // an overlong '/'
		"\xC1\xBF",                  // an overlong sequence
		"\xE0\x9F\xBF",              // an overlong three-byte sequence
		"\xF0\x8F\xBF\xBF",          // an overlong four-byte sequence
		"\xED\xA0\x80",              // U+D800, a surrogate half
		"\xED\xBF\xBF",              // U+DFFF, the other end
		"\xF4\x90\x80\x80",          // beyond U+10FFFF
		"\xF5\x80\x80\x80",          // beyond the four-byte range entirely
		"\xFE",                      // never valid
		"\xFF",                      // never valid
		"\xE2\x82",                  // truncated three-byte sequence
		"\xF0\x9F\x98",              // truncated four-byte sequence
		"ok\xE2\x82",                // truncated at the very end
	};

	for (const auto& bytes : CASES)
	{
		GuestBytes buffer(guest.machine, bytes);
		const bool guest_says =
			guest->vmcall("rust_string_is_utf8", buffer.address(), buffer.size()) != 0;

		INFO("Case of " << bytes.size() << " bytes, index "
			<< (&bytes - CASES.data()));
		REQUIRE(RustString::is_valid_utf8(bytes) == guest_says);
	}
}

// GuestRustMinCapacity has three branches - 8 for a single byte, 4 for an
// element up to 1024 bytes, and 1 above that - and the growth test above only
// reaches the first two, through String and Vec<u32>.
TEST_CASE("Rust RawVec minimum capacity per element size", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	struct Big { uint8_t data[2048]; };

	REQUIRE(RustVec<uint8_t>::MIN_CAPACITY == 8);
	REQUIRE(RustVec<uint64_t>::MIN_CAPACITY == 4);
	REQUIRE(RustVec<Big>::MIN_CAPACITY == 1);

	for (const size_t pushes : {1u, 2u, 3u, 5u, 9u, 17u})
	{
		INFO("After " << pushes << " pushes");
		{
			ScopedRustVec<uint8_t> vec(guest.machine);
			for (size_t i = 0; i < pushes; i++)
				vec->push_back(guest.machine, uint8_t(i));
			REQUIRE(vec->capacity()
				== guest->vmcall("rust_u8vec_capacity_after", pushes));
		}
		{
			ScopedRustVec<uint64_t> vec(guest.machine);
			for (size_t i = 0; i < pushes; i++)
				vec->push_back(guest.machine, uint64_t(i));
			REQUIRE(vec->capacity()
				== guest->vmcall("rust_u64vec_capacity_after", pushes));
		}
	}

	// A large element gets no minimum at all, so the first push allocates one
	for (const size_t pushes : {1u, 2u, 3u})
	{
		INFO("After " << pushes << " large pushes");
		ScopedRustVec<Big> vec(guest.machine);
		for (size_t i = 0; i < pushes; i++)
			vec->push_back(guest.machine, Big{});
		REQUIRE(vec->capacity()
			== guest->vmcall("rust_bigvec_capacity_after", pushes));
	}
}

// The remaining GuestRustVec operations, checked against the guest where it
// can see them, and against the arena where it cannot.
TEST_CASE("Rust Vec host API surface", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	guest.warmup();
	const unsigned allocs_before = guest.live_allocations();

	{
		// The count constructor value-initializes its elements
		ScopedRustVec<uint32_t> vec(guest.machine, size_t(4));
		REQUIRE(vec->size() == 4);
		REQUIRE(vec->capacity() >= 4);
		REQUIRE(vec->to_vector(guest.machine) == std::vector<uint32_t>{ 0, 0, 0, 0 });
		REQUIRE(vec->size_bytes() == 4 * sizeof(uint32_t));
		REQUIRE(vec->capacity_bytes() == vec->capacity() * sizeof(uint32_t));

		// assign() replaces the contents, reusing the allocation when it fits
		const auto data = vec->data();
		vec->assign(guest.machine, std::vector<uint32_t>{ 5, 6, 7 });
		REQUIRE(vec->size() == 3);
		REQUIRE(vec->data() == data);
		REQUIRE(guest->vmcall("rust_vec_sum", vec) == 18);

		// address_at() and at() are the guest address, and the element itself
		REQUIRE(vec->address_at(0) == vec->data());
		REQUIRE(vec->address_at(2) == vec->data() + 2 * sizeof(uint32_t));
		REQUIRE(vec->at(guest.machine, 1) == 6);
		REQUIRE_THROWS(vec->address_at(3));
		REQUIRE_THROWS(vec->at(guest.machine, 3));

		// Iterating reads the elements straight out of guest memory
		uint64_t sum = 0;
		for (auto it = vec->begin(guest.machine); it != vec->end(guest.machine); ++it)
			sum += *it;
		REQUIRE(sum == 18);
#ifdef RISCV_SPAN_AVAILABLE
		REQUIRE(vec->to_span(guest.machine).size() == 3);
		REQUIRE(vec->to_span(guest.machine)[2] == 7);
#endif

		// pop_back() drops the last element and keeps the allocation
		const auto capacity = vec->capacity();
		vec->pop_back(guest.machine);
		REQUIRE(vec->size() == 2);
		REQUIRE(vec->capacity() == capacity);
		REQUIRE(guest->vmcall("rust_vec_sum", vec) == 11);

		// resize() growing value-initializes what it adds
		vec->resize(guest.machine, 5);
		REQUIRE(vec->to_vector(guest.machine) == std::vector<uint32_t>{ 5, 6, 0, 0, 0 });
		REQUIRE(guest->vmcall("rust_vec_sum", vec) == 11);

		// reserve() only ever grows
		vec->reserve(guest.machine, 64);
		REQUIRE(vec->capacity() >= 64);
		const auto grown = vec->capacity();
		vec->reserve(guest.machine, 2);
		REQUIRE(vec->capacity() == grown);
		REQUIRE(vec->size() == 5);

		// An empty vector has nothing left to pop
		vec->clear(guest.machine);
		REQUIRE(vec->empty());
		REQUIRE_THROWS(vec->pop_back(guest.machine));

		// A slice of the whole vector, and the empty borrows: dangling but
		// never null, like the collections themselves
		vec->assign(guest.machine, std::vector<uint32_t>{ 1, 2 });
		REQUIRE(vec->as_slice().size() == 2);
		const RustSlice<uint32_t> empty_slice;
		REQUIRE(empty_slice.empty());
		REQUIRE(empty_slice.address() == RustVec<uint32_t>::DANGLING);
	}

	REQUIRE(guest.live_allocations() == allocs_before);
}

// The remaining GuestRustString operations. The capacity is what says whether
// an allocation is being reused or replaced, as the length alone cannot.
TEST_CASE("Rust String host API surface", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	guest.warmup();
	const unsigned allocs_before = guest.live_allocations();

	{
		ScopedRustString str(guest.machine, "initial");
		REQUIRE(str->size() == 7);
		REQUIRE(str->length() == str->size());

		// set_string() reuses the allocation when the new text fits in it
		const auto capacity = str->capacity();
		const auto data = str->data();
		str->set_string(guest.machine, "short");
		REQUIRE(str->to_view(guest.machine) == "short");
		REQUIRE(str->capacity() == capacity);
		REQUIRE(str->data() == data);

		// clear() drops the characters and keeps the allocation, so the guest
		// sees an empty string that still owns its bytes
		str->clear(guest.machine);
		REQUIRE(str->empty());
		REQUIRE(str->capacity() == capacity);
		REQUIRE(str->data() == data);
		REQUIRE(guest->vmcall("rust_string_len", str) == 0);

		// ... and it can be filled again without allocating anything new
		str->append(guest.machine, "back");
		REQUIRE(str->to_view(guest.machine) == "back");
		REQUIRE(str->data() == data);

		// reserve() only ever grows
		str->reserve(guest.machine, 256);
		REQUIRE(str->capacity() >= 256);
		const auto grown = str->capacity();
		str->reserve(guest.machine, 1);
		REQUIRE(str->capacity() == grown);
		REQUIRE(str->to_view(guest.machine) == "back");
		REQUIRE(guest->vmcall("rust_string_checksum", str) == fnv1a("back"));

		// A borrowed &str can be copied out as an owned std::string, and an
		// empty borrow is dangling rather than null
		const RustStr borrowed = str->as_str();
		REQUIRE(borrowed.to_string(guest.machine) == "back");
		const RustStr empty_str;
		REQUIRE(empty_str.empty());
		REQUIRE(empty_str.address() == RustString::DANGLING);
		REQUIRE(empty_str.to_string(guest.machine).empty());
	}

	REQUIRE(guest.live_allocations() == allocs_before);
}

// Every element of a Vec<String> owns a guest allocation of its own, so each
// operation that removes one has to release it. A Vec<u32> cannot catch a
// mistake here, because dropping an u32 does nothing at all.
TEST_CASE("Rust Vec<String> releases the elements it drops", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	static const std::vector<std::string> NAMES {
		"a string long enough to need its own allocation",
		"and another one just like it",
		"and a third",
	};

	guest.warmup();
	const unsigned allocs_before = guest.live_allocations();

	{
		ScopedRustVec<RustString> names(guest.machine, NAMES);
		// The scoped object itself, the element array, and one per string
		REQUIRE(guest.live_allocations() == allocs_before + 2 + NAMES.size());

		// pop_back() releases the last element's bytes
		names->pop_back(guest.machine);
		REQUIRE(names->size() == 2);
		REQUIRE(guest.live_allocations() == allocs_before + 4);

		// resize() shrinking releases everything it drops
		names->resize(guest.machine, 1);
		REQUIRE(guest.live_allocations() == allocs_before + 3);

		// resize() growing leaves empty Strings, which own nothing yet
		names->resize(guest.machine, 3);
		REQUIRE(names->size() == 3);
		REQUIRE(guest.live_allocations() == allocs_before + 3);
		REQUIRE(names->at(guest.machine, 2).empty());
		REQUIRE(names->at(guest.machine, 2).data() == RustString::DANGLING);
		REQUIRE(names->to_string_vector(guest.machine)
			== std::vector<std::string>{ NAMES[0], "", "" });

		// clear() releases them all, and keeps the array
		names->clear(guest.machine);
		REQUIRE(names->empty());
		REQUIRE(guest.live_allocations() == allocs_before + 2);

		// assign() replaces the contents, releasing the old elements
		names->assign(guest.machine, NAMES);
		REQUIRE(names->to_string_vector(guest.machine) == NAMES);
	}

	REQUIRE(guest.live_allocations() == allocs_before);
}

// ===========================================================================
// Box, the guest Rust enum, and the attribute tree
//
// The examples come first again: each shows one way a structured value crosses
// the boundary. What makes all of it work is that the guest's global allocator
// is the host arena, so a tree the host builds is a tree the guest owns.
// ===========================================================================

// Box<T> is the only Rust smart pointer with a guaranteed layout: one non-null
// pointer, ABI-compatible with *mut T. Because the block the host allocates
// really did come from the guest's global allocator, Box::from_raw() on it is
// sound rather than merely convenient - and the guest's ordinary drop returns
// it to the arena, which the allocation balance below proves.
TEST_CASE("Example: a Box crosses the boundary in both directions", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	guest.warmup();
	const auto allocs_before = guest.live_allocations();

	// The host builds a Box<i64> and gives it away. release() hands over
	// ownership without freeing: from here on it is the guest's to drop.
	RustBox<int64_t> boxed(guest.machine, int64_t(4321));
	REQUIRE(!boxed.empty());
	REQUIRE(boxed.get(guest.machine) == 4321);
	REQUIRE(guest.live_allocations() == allocs_before + 1);

	REQUIRE(guest->vmcall("rust_box_take_i64", boxed.release()) == 4321);
	// The guest dropped it, so the arena is back where it started
	REQUIRE(guest.live_allocations() == allocs_before);

	// And the other way: the guest allocates, the host takes ownership
	const auto addr = guest->vmcall("rust_box_make_i64", int64_t(21));
	REQUIRE(guest.live_allocations() == allocs_before + 1);

	RustBox<int64_t> from_guest;
	from_guest.assume_ownership(guest.machine, addr);
	REQUIRE(from_guest.get(guest.machine) == 42);
	from_guest.free(guest.machine);
	REQUIRE(guest.live_allocations() == allocs_before);
}

// The two unsized boxes, Box<[T]> and Box<str>, are the same fat pointers as a
// slice and a &str, with ownership. The host builds a struct of both, hands
// over the one pointer, and the guest's drop glue frees all three allocations.
TEST_CASE("Example: the host builds a Box<[T]> and a Box<str>", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	guest.warmup();
	const auto allocs_before = guest.live_allocations();

	static const std::vector<uint32_t> NUMBERS { 5, 10, 15, 20 };
	static const std::string NAME = "a boxed str the host allocated";

	// #[repr(C)] struct Boxes { numbers: Box<[u32]>, name: Box<str> }
	struct HostBoxes {
		RustBoxedSlice<uint32_t> numbers;
		RustBoxedStr name;
	};
	static_assert(sizeof(HostBoxes) == 4 * sizeof(uint64_t), "Two fat pointers");

	const auto addr = guest->arena().malloc(sizeof(HostBoxes));
	REQUIRE(addr != 0);
	auto& boxes = *guest->memory.memarray<HostBoxes>(addr, 1);
	new (&boxes) HostBoxes();
	boxes.numbers.assign(guest.machine, NUMBERS);
	boxes.name.set_string(guest.machine, NAME);

	REQUIRE(boxes.numbers.size() == NUMBERS.size());
	REQUIRE(boxes.numbers.to_vector(guest.machine) == NUMBERS);
	REQUIRE(boxes.name.to_string(guest.machine) == NAME);
	// The struct itself, the boxed slice and the boxed str
	REQUIRE(guest.live_allocations() == allocs_before + 3);

	uint64_t expected = fnv1a(NAME);
	for (const uint32_t n : NUMBERS)
		expected = expected * 131 + n;

	REQUIRE(guest->vmcall("rust_boxes_take", addr) == expected);
	// One from_raw and one drop released all three
	REQUIRE(guest.live_allocations() == allocs_before);
}

// The attribute tree, host to guest. The host builds the whole thing on the
// guest heap in its final shape - one allocation for all the entries of a map,
// not a node per key - and the guest takes it with a single from_raw. There is
// no rebuilding on arrival, and no host-side teardown function either: the
// guest's drop glue walks the tree, including the nested group and list.
TEST_CASE("Example: the host builds an attribute tree and the guest takes it", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	guest.warmup();
	const auto allocs_before = guest.live_allocations();

	// A Box<Attributes>: the object the guest will call from_raw() on
	RustGroup root;
	RustAttrs& attrs = root.emplace(guest.machine);
	host_build_attrs(guest.machine, attrs);

	REQUIRE(attrs.size() == 7);
	REQUIRE(attrs.is_sorted(guest.machine));
	const uint64_t expected = host_checksum_attrs(guest.machine, attrs);

	// The guest computes the same checksum over the same bytes, and drops the
	// tree on its way out
	REQUIRE(guest->vmcall("rust_attrs_take", root.release()) == expected);
	REQUIRE(guest.live_allocations() == allocs_before);
}

// The same tree, but the host keeps it and only lends it out for the call. This
// is the shape an engine handing attributes to a script every frame wants:
// build once, pass the same address every time, allocate nothing per call.
TEST_CASE("Example: the guest borrows a host-owned attribute tree", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	guest.warmup();
	const auto allocs_before = guest.live_allocations();

	{
		ScopedRustAttrs attrs(guest.machine);
		host_build_attrs(guest.machine, *attrs);
		const uint64_t expected = host_checksum_attrs(guest.machine, *attrs);

		// Called twice with the same address: nothing is allocated or freed
		const auto allocs_built = guest.live_allocations();
		REQUIRE(guest->vmcall("rust_attrs_borrow", attrs.address()) == expected);
		REQUIRE(guest->vmcall("rust_attrs_borrow", attrs.address()) == expected);
		REQUIRE(guest.live_allocations() == allocs_built);
	}
	// The ScopedArenaObject freed the tree, recursively
	REQUIRE(guest.live_allocations() == allocs_before);
}

// The guest builds a tree of its own and lends it to the host, which walks it
// in place. Handing over the address of its own three words is all the guest
// does, no matter how big the tree is.
TEST_CASE("Example: the guest lends its attribute tree to the host", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	guest.warmup();
	const auto allocs_before = guest.live_allocations();

	g_lent_attrs_checksum = 0;
	g_lent_attrs_size = 0;

	// The guest returns the checksum it computed over its own tree, and the
	// system call handler stored the one the host computed over the same bytes
	const auto guest_checksum = guest->vmcall("rust_attrs_build");
	REQUIRE(g_lent_attrs_size == 7);
	REQUIRE(g_lent_attrs_checksum == guest_checksum);

	// The guest still owned it and dropped it at the end of the function
	REQUIRE(guest.live_allocations() == allocs_before);
}

// The host filling in a tree the guest owns, which is the same direction as
// GuestRustString::set_string() but for a whole tree. What the guest gets back
// is its own Attributes: it can insert into it and it drops all of it.
TEST_CASE("Example: the host fills in a guest-owned attribute tree", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	guest.warmup();
	const auto allocs_before = guest.live_allocations();

	// The guest checksums what arrived, inserts one more entry, and checksums
	// again - so a non-zero result means the host-built entries survived the
	// guest growing the same vector
	const auto result = guest->vmcall("rust_attrs_host_fills");
	REQUIRE(result != 0);
	REQUIRE(guest.live_allocations() == allocs_before);
}

// A guest that would rather have a real std::HashMap gets one for the price of
// a collect(). Mirroring hashbrown would mean betting on a #[repr(Rust)] table
// layout and a per-process hash seed; a sorted Vec needs neither, and the guest
// keeps the choice of what to turn it into.
TEST_CASE("The guest can collect the tree into a HashMap", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	guest.warmup();
	const auto allocs_before = guest.live_allocations();

	RustGroup root;
	RustAttrs& attrs = root.emplace(guest.machine);
	host_build_attrs(guest.machine, attrs);

	uint64_t expected = attrs.size();
	for (const char* key : { "alpha", "beta", "gamma" })
		expected += host_checksum_value(guest.machine, attrs.at(guest.machine, key));

	REQUIRE(guest->vmcall("rust_attrs_into_hashmap", root.release()) == expected);
	// The HashMap the guest built owns the keys and values now, and dropping
	// it released everything the host allocated
	REQUIRE(guest.live_allocations() == allocs_before);
}

// ===========================================================================
// Verification of the new host mirrors
// ===========================================================================

// A #[repr(C, u64)] enum is a layout the guest requests, not one the host had
// to discover - but only rustc can say whether it delivered it. This asks the
// guest for the tag values, the payload offsets and the sizes, so that a
// release which lays the enum out differently fails here.
TEST_CASE("Rust enum layout matches the host mirror", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;

	static constexpr std::size_t WORDS = 21;
	const auto addr = guest->arena().malloc(WORDS * sizeof(uint64_t));
	guest->vmcall("rust_probe_enum_layout", addr);
	const auto* w = guest->memory.memarray<const uint64_t>(addr, WORDS);

	// The enum is the tag plus the widest variant, aligned like the payload
	REQUIRE(w[0] == sizeof(RustValue));
	REQUIRE(w[1] == alignof(RustValue));

	// The discriminants are the declaration order, which is what the order of
	// the host's variant list assumes
	REQUIRE(w[2] == RustValue::index_of<std::monostate>());
	REQUIRE(w[3] == RustValue::index_of<int64_t>());
	REQUIRE(w[4] == RustValue::index_of<double>());
	REQUIRE(w[5] == RustValue::index_of<bool>());
	REQUIRE(w[6] == RustValue::index_of<RustString>());
	REQUIRE(w[7] == RustValue::index_of<RustGroup>());
	REQUIRE(w[8] == RustValue::index_of<RustList>());

	// Every payload starts at the same offset, right after the tag. This is
	// the reading that a tag as wide as the payload alignment guarantees: a
	// narrower tag would leave it ambiguous whether Bool sits at offset 1.
	REQUIRE(w[9]  == offsetof(RustValue, payload));
	REQUIRE(w[10] == offsetof(RustValue, payload));
	REQUIRE(w[11] == offsetof(RustValue, payload));
	REQUIRE(w[12] == offsetof(RustValue, payload));
	REQUIRE(w[13] == offsetof(RustValue, payload));
	REQUIRE(w[14] == offsetof(RustValue, payload));
	REQUIRE(w[9] == sizeof(RustValue::tag_type));

	// The entry struct is a String followed by a Value, in declaration order,
	// which #[repr(C)] promises and a tuple would not have
	using HostAttr = GuestRustAttr<RISCV64, RustValue>;
	REQUIRE(w[15] == sizeof(HostAttr));
	REQUIRE(w[16] == offsetof(HostAttr, key));
	REQUIRE(w[17] == offsetof(HostAttr, value));

	// Attributes is a newtype over the Vec of entries, and a Box is one word
	REQUIRE(w[18] == sizeof(RustAttrs));
	REQUIRE(w[19] == sizeof(RustGroup));
	REQUIRE(w[20] == 0);

	guest->arena().free(addr);
}

// The host side of the map on its own: sorted insertion, lookup, overwrite and
// erase, with the allocation balance checked after every step. None of this
// needs the guest, but all of it happens in guest memory.
TEST_CASE("Rust attributes: insertion, lookup and erase", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	guest.warmup();
	const auto allocs_before = guest.live_allocations();

	{
		ScopedRustAttrs attrs(guest.machine);
		REQUIRE(attrs->empty());
		REQUIRE(attrs->find(guest.machine, "nothing") == nullptr);

		// Inserted out of order, and kept in key order
		attrs->insert_or_assign(guest.machine, "gamma", int64_t(3));
		attrs->insert_or_assign(guest.machine, "alpha", int64_t(1));
		attrs->insert_or_assign(guest.machine, "beta", int64_t(2));
		REQUIRE(attrs->size() == 3);
		REQUIRE(attrs->is_sorted(guest.machine));

		std::vector<std::string> keys;
		attrs->for_each(guest.machine, [&] (std::string_view key, const RustValue&) {
			keys.emplace_back(key);
		});
		REQUIRE(keys == std::vector<std::string>{ "alpha", "beta", "gamma" });

		// The value type picks the variant from the host value, the way the
		// converting constructor of std::variant does
		REQUIRE(attrs->at(guest.machine, "alpha").holds_alternative<int64_t>());
		attrs->insert_or_assign(guest.machine, "alpha", std::string("now a string"));
		REQUIRE(attrs->at(guest.machine, "alpha").holds_alternative<RustString>());
		REQUIRE(attrs->at(guest.machine, "alpha").get<RustString>().to_string(guest.machine)
			== "now a string");
		REQUIRE(attrs->size() == 3);

		// Overwriting releases what the old variant owned
		const auto allocs_with_string = guest.live_allocations();
		attrs->insert_or_assign(guest.machine, "alpha", 1.5);
		REQUIRE(guest.live_allocations() == allocs_with_string - 1);
		REQUIRE(attrs->at(guest.machine, "alpha").get<double>() == 1.5);

		// Erase closes the gap and keeps the order
		REQUIRE(attrs->erase(guest.machine, "beta"));
		REQUIRE(!attrs->erase(guest.machine, "beta"));
		REQUIRE(attrs->size() == 2);
		REQUIRE(attrs->is_sorted(guest.machine));
		REQUIRE(attrs->contains(guest.machine, "alpha"));
		REQUIRE(attrs->contains(guest.machine, "gamma"));
		REQUIRE_THROWS_AS(attrs->at(guest.machine, "beta"), std::out_of_range);

		// A whole host map at once, in one allocation for the entries
		const std::map<std::string, int64_t> HOST_MAP {
			{ "one", 1 }, { "two", 2 }, { "three", 3 } };
		attrs->assign(guest.machine, HOST_MAP);
		REQUIRE(attrs->size() == 3);
		REQUIRE(attrs->is_sorted(guest.machine));
		REQUIRE(attrs->at(guest.machine, "two").get<int64_t>() == 2);

		attrs->clear(guest.machine);
		REQUIRE(attrs->empty());
	}

	REQUIRE(guest.live_allocations() == allocs_before);
}

// free() on a tree walks it: the group, the list, every string in both. This is
// the host-side counterpart of the guest's drop glue, and the reason there is
// no hand-written teardown function anywhere in this file.
TEST_CASE("Rust attributes free the whole tree recursively", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	guest.warmup();
	const auto allocs_before = guest.live_allocations();

	RustAttrs attrs;
	host_build_attrs(guest.machine, attrs);
	REQUIRE(guest.live_allocations() > allocs_before);

	// One call, and the entries, the keys, the strings, the nested group and
	// the list are all gone
	attrs.free(guest.machine);
	REQUIRE(guest.live_allocations() == allocs_before);
	REQUIRE(attrs.empty());

	// A freed map is a valid empty one, so freeing twice is not a double free
	attrs.free(guest.machine);
	REQUIRE(guest.live_allocations() == allocs_before);
}

// A Rust enum has no valueless state: free() has to leave something the guest
// can still drop, which is the first variant in its default state.
TEST_CASE("A freed Rust enum holds its first variant", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	guest.warmup();
	const auto allocs_before = guest.live_allocations();

	RustValue value;
	REQUIRE(value.holds_alternative<std::monostate>());
	REQUIRE(value.valid());

	value.set(guest.machine, std::string("a heap allocated string"));
	REQUIRE(value.holds_alternative<RustString>());
	REQUIRE(guest.live_allocations() == allocs_before + 1);

	value.free(guest.machine);
	// Back to Nil, which owns nothing, so a second free changes nothing
	REQUIRE(value.holds_alternative<std::monostate>());
	REQUIRE(guest.live_allocations() == allocs_before);
	value.free(guest.machine);
	REQUIRE(guest.live_allocations() == allocs_before);

	// Every variant can be selected explicitly, and to_variant() is only
	// available when no variant is a nested container
	value.emplace<int64_t>(guest.machine, int64_t(-5));
	REQUIRE(value.get<int64_t>() == -5);
	REQUIRE(value.get_if<double>() == nullptr);
	REQUIRE_THROWS_AS(value.get<double>(), std::runtime_error);
	REQUIRE(value.index() == RustValue::index_of<int64_t>());
}

// A guest is adversarial input: it can hand over a map whose length exceeds its
// capacity, or whose keys are out of order. Neither may corrupt the host, and
// neither may go unnoticed.
TEST_CASE("Rust attributes reject a malformed guest map", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	guest.warmup();
	const auto allocs_before = guest.live_allocations();

	RustAttrs attrs;
	attrs.insert_or_assign(guest.machine, "alpha", int64_t(1));
	attrs.insert_or_assign(guest.machine, "beta", int64_t(2));
	attrs.validate(guest.machine);
	REQUIRE(attrs.is_sorted(guest.machine));

	// A length beyond the capacity is caught before anything is read
	const auto real_len = attrs.entries.len;
	attrs.entries.len = attrs.entries.cap + 1;
	REQUIRE_THROWS_AS(attrs.validate(guest.machine), std::runtime_error);
	attrs.entries.len = real_len;

	// So is a map that claims more entries than the caller allows
	REQUIRE_THROWS_AS(attrs.validate(guest.machine, 1), std::runtime_error);
	attrs.validate(guest.machine, 2);

	// Keys out of order cannot corrupt anything - a binary search that misses
	// only fails to find - but is_sorted() reports it
	auto* array = attrs.entries.as_array(guest.machine);
	std::swap(array[0], array[1]);
	REQUIRE(!attrs.is_sorted(guest.machine));

	attrs.free(guest.machine);
	REQUIRE(guest.live_allocations() == allocs_before);
}

// An attribute map whose values are all plain values or strings can be copied
// to the host in one call. A tree with nested groups cannot - to_variant() is
// compiled out for those, so for_each() is the only way in - which is what
// keeps a nested read from silently returning a pointer as an integer.
TEST_CASE("A flat Rust attribute map can be copied to the host", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	guest.warmup();
	const auto allocs_before = guest.live_allocations();

	using FlatValue = GuestRustEnum<RISCV64, uint64_t,
		std::monostate, int64_t, double, RustString>;
	using FlatAttrs = GuestRustAttributes<RISCV64, FlatValue>;
	// The same shape as the recursive value, minus the two boxed variants
	static_assert(sizeof(FlatValue) == 8 + sizeof(RustString), "tag + String");

	{
		ScopedArenaObject<RISCV64, FlatAttrs> attrs(guest.machine);
		attrs->insert_or_assign(guest.machine, "count", int64_t(7));
		attrs->insert_or_assign(guest.machine, "ratio", 0.25);
		attrs->insert_or_assign(guest.machine, "label", std::string("a label"));
		attrs->emplace<std::monostate>(guest.machine, "nothing");

		// A single variant on its own
		const auto value = attrs->at(guest.machine, "count").to_variant(guest.machine);
		REQUIRE(std::holds_alternative<int64_t>(value));
		REQUIRE(std::get<int64_t>(value) == 7);

		// And the whole map at once, with the guest strings copied out
		const auto map = attrs->to_map(guest.machine);
		REQUIRE(map.size() == 4);
		REQUIRE(std::get<int64_t>(map.at("count")) == 7);
		REQUIRE(std::get<double>(map.at("ratio")) == 0.25);
		REQUIRE(std::get<std::string>(map.at("label")) == "a label");
		REQUIRE(std::holds_alternative<std::monostate>(map.at("nothing")));
	}

	REQUIRE(guest.live_allocations() == allocs_before);
}

// ===========================================================================
// Block memory operations
//
// The guest is linked with --wrap=memcpy and friends, so a Rust program that
// never mentions memcpy still ends up in the host for every bulk copy it makes
// - and it makes a great many, because that is where Vec growth, slice copies
// and String appends all end up.
//
// Two things are checked for each of them: that the bytes the host produced
// are the right bytes, and that the operation reached the host at all. The
// second one matters, because a guest that quietly kept its own memcpy would
// pass every correctness check in this file while emulating the whole loop.
// ===========================================================================

/// @brief The byte pattern the guest fills its buffers with, so the host can
/// build the same bytes and check the checksum of what came back.
static std::string memop_pattern(size_t len)
{
	std::string bytes(len, '\0');
	for (size_t i = 0; i < len; i++)
		bytes[i] = char(i % 251);
	return bytes;
}

TEST_CASE("Booting a Rust guest goes through the host memory syscalls", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	MemoryOpCounter ops;
	RustGuest guest;

	// glibc's startup and the Rust runtime both copy and zero a great deal
	// before main() is reached, and --wrap catches all of it
	INFO("boot: memcpy=" << ops[MEMCPY] << " memset=" << ops[MEMSET]
		<< " memmove=" << ops[MEMMOVE] << " memcmp=" << ops[MEMCMP]);
	REQUIRE(ops[MEMCPY] > 0);
	REQUIRE(ops[MEMSET] > 0);
}

TEST_CASE("Rust memset runs on the host", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	guest.warmup();
	const unsigned allocs_before = guest.live_allocations();

	for (const size_t len : {1u, 15u, 64u, 4096u, 256u * 1024u})
	{
		MemoryOpCounter ops;
		const uint64_t checksum = guest->vmcall("rust_memset_fill", len, uint8_t(0xA5));

		INFO("memset of " << len << " bytes");
		REQUIRE(checksum == fnv1a(std::string(len, char(0xA5))));
		REQUIRE(ops[MEMSET] > 0);
	}

	REQUIRE(guest.live_allocations() == allocs_before);
}

TEST_CASE("Rust memcpy runs on the host", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	guest.warmup();
	const unsigned allocs_before = guest.live_allocations();

	for (const size_t len : {1u, 15u, 64u, 4096u, 256u * 1024u})
	{
		MemoryOpCounter ops;
		const uint64_t checksum = guest->vmcall("rust_memcpy_roundtrip", len);

		INFO("memcpy of " << len << " bytes");
		REQUIRE(checksum == fnv1a(memop_pattern(len)));
		REQUIRE(ops[MEMCPY] > 0);
	}

	REQUIRE(guest.live_allocations() == allocs_before);
}

TEST_CASE("Rust memmove runs on the host, both directions", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	guest.warmup();
	const unsigned allocs_before = guest.live_allocations();

	// The host has one path for a destination below the source and another
	// for a destination above it, and only the second one has to copy
	// backwards. Both are exercised, with an overlap that is not a multiple
	// of the word size as well as one that is.
	for (const size_t len : {64u, 4096u, 256u * 1024u})
	{
		for (const size_t shift : {1u, 7u, 8u, 32u})
		{
			for (const bool downwards : {true, false})
			{
				MemoryOpCounter ops;
				const uint64_t checksum =
					guest->vmcall("rust_memmove_overlap", len, shift, downwards);

				std::string expected = memop_pattern(len);
				if (downwards) // copy_within(shift.., 0)
					expected.replace(0, len - shift, expected.substr(shift));
				else           // copy_within(..len - shift, shift)
					expected.replace(shift, len - shift, expected.substr(0, len - shift));

				INFO("memmove of " << len << " bytes shifted " << shift
					<< (downwards ? " down" : " up"));
				REQUIRE(checksum == fnv1a(expected));
				REQUIRE(ops[MEMMOVE] > 0);
			}
		}
	}

	REQUIRE(guest.live_allocations() == allocs_before);
}

TEST_CASE("Rust memcmp runs on the host", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	guest.warmup();
	const unsigned allocs_before = guest.live_allocations();

	const size_t len = 4096;
	const std::string pattern = memop_pattern(len);

	// An index past the end leaves the two buffers equal; otherwise the byte
	// at that index decides the order, and the host has to return a value of
	// the right sign for Ord to agree with the host's own comparison
	struct { size_t index; uint8_t value; int expected; } cases[] {
		{ len,      0x00, 0 },   // untouched: the two are equal
		{ 0,        0xFF, -1 },  // right is larger at the very first byte
		{ 0,        0x00, 0 },   // pattern[0] is already 0
		{ len - 1,  0xFF, -1 },  // and at the very last one
		{ len - 1,  0x00, 1 },   // pattern[len-1] is non-zero, so left wins
		{ len / 2,  0xFF, -1 },
	};

	for (const auto& c : cases)
	{
		MemoryOpCounter ops;
		const int64_t order = guest->vmcall("rust_memcmp_order", len, c.index, c.value);

		INFO("memcmp with byte " << c.index << " set to " << int(c.value));
		REQUIRE(order == c.expected);
		REQUIRE(ops[MEMCMP] > 0);
	}

	REQUIRE(guest.live_allocations() == allocs_before);
}

TEST_CASE("Rust slice equality runs on the host", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	guest.warmup();
	const unsigned allocs_before = guest.live_allocations();

	// `a == b` does not compile to memcmp: LLVM emits the equality-only entry
	// point, __memcmpeq, because the caller never looks at the sign. It has to
	// be wrapped separately, and this is the test that says so - wrapping only
	// memcmp leaves the ordinary case emulated while every other test here
	// still passes.
	const size_t len = 4096;

	for (const size_t len_case : {1u, 64u, 4096u})
	{
		MemoryOpCounter ops;
		INFO("equal slices of " << len_case << " bytes");
		// index == len leaves the two buffers identical
		REQUIRE(uint64_t(guest->vmcall("rust_memcmp_equality",
			len_case, len_case, uint8_t(0))) == 1);
		REQUIRE(ops[MEMCMP] > 0);
	}

	// And a difference at either end has to be found
	for (const size_t index : {size_t(0), len / 2, len - 1})
	{
		MemoryOpCounter ops;
		INFO("slices differing at byte " << index);
		REQUIRE(uint64_t(guest->vmcall("rust_memcmp_equality",
			len, index, uint8_t(0xFF))) == 0);
		REQUIRE(ops[MEMCMP] > 0);
	}

	REQUIRE(guest.live_allocations() == allocs_before);
}

TEST_CASE("Rust block memory operations handle the edges", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	guest.warmup();
	const unsigned allocs_before = guest.live_allocations();

	// A zero length has to be accepted and do nothing. Rust may or may not
	// make the call at all, so only the result is checked here.
	REQUIRE(uint64_t(guest->vmcall("rust_memset_fill", 0u, uint8_t(0xA5)))
		== fnv1a(""));
	REQUIRE(uint64_t(guest->vmcall("rust_memcpy_roundtrip", 0u)) == fnv1a(""));

	// An overlap of the whole buffer: source and destination are the same
	// address, which is the degenerate case of the backwards path
	const size_t len = 1024;
	for (const bool downwards : {true, false}) {
		INFO("memmove onto itself, " << (downwards ? "down" : "up"));
		REQUIRE(uint64_t(guest->vmcall("rust_memmove_overlap", len, 0u, downwards))
			== fnv1a(memop_pattern(len)));
	}

	// A one-byte overlap, where the copy is almost the whole buffer
	std::string expected = memop_pattern(len);
	expected.replace(0, len - 1, expected.substr(1));
	REQUIRE(uint64_t(guest->vmcall("rust_memmove_overlap", len, 1u, true))
		== fnv1a(expected));

	// Two empty slices compare equal
	REQUIRE(int64_t(guest->vmcall("rust_memcmp_order", 0u, 0u, uint8_t(0))) == 0);

	REQUIRE(guest.live_allocations() == allocs_before);
}

TEST_CASE("A Rust String grown through the host memcpy", "[RustNative]")
{
	if (!rust_toolchain_available())
		SKIP("No riscv64gc-unknown-linux-gnu Rust toolchain available");

	RustGuest guest;
	guest.warmup();
	const unsigned allocs_before = guest.live_allocations();

	{
		// The ordinary case, and the reason this matters at all: a Rust
		// program that never mentions memcpy still spends most of its bulk
		// memory work inside one. The String is built in the shared arena and
		// read back by the host without a copy.
		const size_t repeats = 64;
		const std::string piece = "The guest appends this through the host's memcpy. ";

		MemoryOpCounter ops;
		ScopedRustString text(guest.machine);
		guest->vmcall("rust_memops_build_string", text, repeats);

		std::string expected;
		for (size_t i = 0; i < repeats; i++)
			expected += piece;

		INFO("String append: memcpy=" << ops[MEMCPY]);
		REQUIRE(text->to_string(guest.machine) == expected);
		REQUIRE(ops[MEMCPY] >= repeats);
	}

	REQUIRE(guest.live_allocations() == allocs_before);
}
