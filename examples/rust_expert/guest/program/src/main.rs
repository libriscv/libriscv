//! A regular Rust program that happens to run inside libriscv.
//!
//! Nothing here is emulator-specific except `mod env`, which points the global
//! allocator at the host's arena. Everything else is ordinary Rust: it calls
//! the host through the generated `host_functions` library, and it exports
//! functions with `#[no_mangle] pub extern "C"` so the host can call back in.
//!
//! Values cross the boundary in their native form. A `String` the host builds
//! is a `String` here, a `Vec<u32>` the guest returns is a `Vec<u32>` the host
//! reads - because both sides allocate out of the same heap, there is nothing
//! to serialize.

// Passing a Rust `String` or `Vec<T>` through extern "C" is the point here:
// the host mirrors their layout instead of serializing them
#![allow(improper_ctypes_definitions)]

mod env;

use core::ffi::c_void;
use host_functions::{data, game, io, math, rpc};

/// The host's create_capture() copies this many bytes of a closure
const MAX_CAPTURE: usize = 32;

/// println! for the host: formats, then hands the bytes to `IO::print`
macro_rules! hprint {
    ($($arg:tt)*) => { io::print(&format!($($arg)*)) };
}

fn main() {
    hprint!("Guest: booting up...");

    let greeting = String::from("Hello from the RISC-V Rust guest!");
    hprint!("Guest: {greeting}");

    let numbers: Vec<i32> = vec![10, 20, 30, 40, 50];
    hprint!("Guest: sum of vector = {}", numbers.iter().sum::<i32>());

    hprint!("Guest: init complete, pausing.");
    // Stop here with the runtime fully initialized, so that the host can start
    // making calls into a warm guest
    env::fast_exit(0);
}

// ---------------------------------------------------------------------------
// The init phase. Game::init_world is listed under "initialization" in the
// JSON, so the host stubs it out after on_init() returns.
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn on_init() {
    hprint!("Guest on_init: calling Game::init_world...");
    game::init_world("TestWorld");
    hprint!("Guest on_init: world initialized.");
}

#[no_mangle]
pub extern "C" fn test_init_only_at_runtime() {
    hprint!("Guest: calling an init-only host function at runtime...");
    game::init_world("ShouldFail");
}

// ---------------------------------------------------------------------------
// The generated host functions, called the way any Rust library is called
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn test_math() -> i32 {
    let sum = math::add(17, 25);
    hprint!("Guest: math::add(17, 25) = {sum}");

    let product = math::multiply(6, 7);
    hprint!("Guest: math::multiply(6, 7) = {product}");

    sum + product
}

#[no_mangle]
pub extern "C" fn test_io() {
    hprint!("Hello from the guest via IO::print!");
}

#[no_mangle]
pub extern "C" fn test_get_time() -> f64 {
    let t = game::get_time();
    hprint!("Guest: game::get_time() = {t:.2}");
    t
}

// ---------------------------------------------------------------------------
// Host functions that hand the guest a Rust collection.
//
// `Data::fill_string` is declared in the JSON as taking a `rust_string_t*`,
// which the generator turns into `&mut String`. The host writes into it with
// GuestRustString, and what comes back is a String this guest owns: it can
// grow it, and it drops it on the way out of this function.
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn test_host_fills() -> i32 {
    let mut text = String::new();
    data::fill_string(&mut text);
    hprint!("Guest: the host filled in a String: '{text}'");

    text.push_str(" ...and the guest appended to it");
    hprint!("Guest: after appending: '{text}' ({} bytes)", text.len());

    let mut values: Vec<u32> = Vec::new();
    data::fill_vector(&mut values);
    hprint!("Guest: the host filled in a Vec<u32> of {} elements", values.len());

    values.push(1000);
    values.iter().sum::<u32>() as i32
}

// ---------------------------------------------------------------------------
// Functions the host calls, taking and returning Rust values
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn compute(a: i32, b: i32) -> i32 {
    hprint!("Guest compute({a}, {b}) = {}", a + b);
    a + b
}

/// A borrowed `&str`: the RISC-V ABI puts the address and the length in two
/// separate registers, so the host passes two vmcall arguments.
#[no_mangle]
pub extern "C" fn greet(ptr: *const u8, len: usize) {
    let name = unsafe { core::str::from_utf8(core::slice::from_raw_parts(ptr, len)) }
        .expect("The host passed bytes that are not UTF-8");
    hprint!("Guest greet: Hello, {name}! (via &str)");
}

/// A `&String`, which is one register holding the address of its three words.
#[no_mangle]
pub extern "C" fn greet_string(name: *const String) {
    let name: &String = unsafe { &*name };
    hprint!("Guest greet_string: Hello, {name}! (via &String)");
}

/// A `String` returned by value. It does not fit in a register, so the caller
/// allocates room for it and passes the address as a hidden first argument -
/// on the host side, a ScopedGuestRustString handed to vmcall() first.
#[no_mangle]
pub extern "C" fn make_greeting() -> String {
    String::from("A String allocated by the guest, owned by the host")
}

#[no_mangle]
pub extern "C" fn sum_vector(data: *const Vec<i32>) -> i32 {
    let data: &Vec<i32> = unsafe { &*data };
    let total: i32 = data.iter().sum();
    hprint!("Guest sum_vector({} elements) = {total}", data.len());
    total
}

// --- Ownership: the guest keeps a String the host allocated ---

static mut STORED: Option<String> = None;

fn stored() -> &'static mut Option<String> {
    unsafe { &mut *core::ptr::addr_of_mut!(STORED) }
}

#[no_mangle]
pub extern "C" fn take_string(s: *mut String) {
    // Move out of the host's String, leaving an empty one behind. The bytes
    // were allocated by the host, and are now the guest's to free.
    let owned = unsafe { core::ptr::read(s) };
    unsafe { core::ptr::write(s, String::new()) };
    hprint!("Guest take_string: received '{owned}', moving it into a static");
    *stored() = Some(owned);
}

#[no_mangle]
pub extern "C" fn print_stored() {
    match stored() {
        Some(s) => hprint!("Guest print_stored: '{s}'"),
        None => hprint!("Guest print_stored: nothing stored"),
    }
}

#[no_mangle]
pub extern "C" fn release_stored() {
    *stored() = None;
}

// ---------------------------------------------------------------------------
// Callbacks. A closure that captures only Copy data is itself Copy and has no
// pointers into the guest stack, so the host can copy its captured bytes out,
// keep them, and call the trampoline with them later.
// ---------------------------------------------------------------------------

extern "C" fn trampoline<F: Fn() + Copy>(data: *mut c_void) {
    unsafe { (*(data as *const F))() }
}

/// Call the closure back through the host, right now.
fn store_and_callback<F: Fn() + Copy>(callback: F) {
    assert!(size_of::<F>() <= MAX_CAPTURE, "Capture too large for the host");
    unsafe {
        rpc::callback(
            trampoline::<F>,
            &callback as *const F as *mut c_void,
            size_of::<F>(),
        )
    }
}

/// Call the closure back in the *other* VM, which runs this same binary and
/// therefore has the trampoline at the same address.
fn invoke_elsewhere<F: Fn() + Copy>(callback: F) -> i64 {
    assert!(size_of::<F>() <= MAX_CAPTURE, "Capture too large for the host");
    unsafe {
        rpc::invoke(
            trampoline::<F>,
            &callback as *const F as *mut c_void,
            size_of::<F>(),
        )
    }
}

#[no_mangle]
pub extern "C" fn test_local_callback() -> i32 {
    let (a, b, c, d) = (42, 99, 123, 456);
    store_and_callback(move || {
        hprint!("  Guest callback: captured values = {a}, {b}, {c}, {d}");
    });
    hprint!("Guest: local callback test complete");
    a + b + c + d
}

// --- RPC between two VMs running the same binary ---

static mut SHARED_COUNTER: i32 = 0;

fn shared_counter() -> &'static mut i32 {
    unsafe { &mut *core::ptr::addr_of_mut!(SHARED_COUNTER) }
}

#[no_mangle]
pub extern "C" fn test_rpc_invoke() -> i32 {
    let delta = 10;
    let (x, y) = (3.14f32, 2.718f32);
    invoke_elsewhere(move || {
        assert_eq!((x, y), (3.14f32, 2.718f32), "RPC captured the wrong floats");
        *shared_counter() += delta;
        hprint!(
            "  Guest RPC target: shared_counter += {delta}, now = {}",
            shared_counter()
        );
    });
    hprint!("Guest: RPC invoke complete");
    0
}

#[no_mangle]
pub extern "C" fn get_shared_counter() -> i32 {
    *shared_counter()
}

// --- The vmcall latency benchmark calls this one ---

#[no_mangle]
pub extern "C" fn increment_counter() {}
