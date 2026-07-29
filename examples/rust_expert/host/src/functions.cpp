/**
 * The host side of host_functions.json.
 *
 * Each registration repeats the signature string from the JSON verbatim. That
 * string, single-spaced and CRC32-hashed, is the only thing tying these
 * implementations to the stubs the generator put in the guest - so a typo here
 * does not produce a subtly wrong call, it produces a resolution failure at
 * load time with the function named.
 *
 * Note that nothing here knows the guest is written in Rust, except the two
 * Data:: functions, which write into collections using the host mirrors of
 * Rust's own String and Vec.
 */
#include "script.hpp"
#include <libriscv/guest/guest_rust_string.hpp>
#include <libriscv/guest/guest_rust_vec.hpp>
#include <time.h>

using gaddr_t = Script::gaddr_t;
using RustString = riscv::GuestRustString<Script::MARCH>;
template <typename T>
using RustVec = riscv::GuestRustVec<Script::MARCH, T>;

static constexpr size_t MAX_CAPTURE = 32;
using CaptureStorage = std::array<uint8_t, MAX_CAPTURE>;

static CaptureStorage create_capture(Script::machine_t& machine, gaddr_t data, size_t size)
{
	if (size > MAX_CAPTURE)
		throw std::runtime_error("Capture data exceeds 32 bytes");
	CaptureStorage capture{};
	machine.memory.memcpy_out(capture.data(), data, size);
	return capture;
}

void Script::register_host_functions()
{
	set_host_function(
		"Math::add",
		"int sys_math_add (int a, int b)",
		[](Script& script) {
			auto [a, b] = script.machine().sysargs<int, int>();
			printf("  [Math::add] %d + %d = %d\n", a, b, a + b);
			script.machine().set_result(a + b);
		});

	set_host_function(
		"Math::multiply",
		"int sys_math_multiply (int a, int b)",
		[](Script& script) {
			auto [a, b] = script.machine().sysargs<int, int>();
			printf("  [Math::multiply] %d * %d = %d\n", a, b, a * b);
			script.machine().set_result(a * b);
		});

	// The guest passes a Rust &str as a pointer and a length, which is what
	// the generated io::print() wrapper turns text.as_ptr()/text.len() into
	set_host_function(
		"IO::print",
		"void sys_print (const char* text, size_t len)",
		[](Script& script) {
			auto [msg] = script.machine().sysargs<std::string_view>();
			printf("  [%s] %.*s\n", script.name().c_str(), (int)msg.size(), msg.data());
		});

	set_host_function(
		"Game::get_time",
		"double sys_game_get_time ()",
		[](Script& script) {
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			script.machine().set_result(ts.tv_sec + ts.tv_nsec * 1e-9);
		});

	// Listed under "initialization" in the JSON: after on_init() returns, the
	// host replaces this entry with a stub that throws
	set_host_function(
		"Game::init_world",
		"void sys_game_init_world (const char* world)",
		[](Script& script) {
			auto [world_name] = script.machine().sysargs<std::string>();
			printf("  [Game::init_world] Initializing world '%s'\n", world_name.c_str());
		});

	// --- Host functions that write into guest-owned Rust collections ---
	//
	// The guest passes &mut String / &mut Vec<u32>, which is the address of
	// the collection's three words. GuestRustString and GuestRustVec are the
	// host's mirrors of those words: assigning through them allocates in the
	// shared arena, so what the guest gets back is a collection it owns, can
	// grow, and drops normally.

	set_host_function(
		"Data::fill_string",
		"void sys_data_fill_string (rust_string_t* out)",
		[](Script& script) {
			auto [self] = script.machine().sysargs<gaddr_t>();
			auto& str = *script.machine().memory.memarray<RustString>(self, 1);
			str.set_string(script.machine(), "A String the host wrote into the guest");
			printf("  [Data::fill_string] wrote %zu bytes at 0x%lx\n",
				str.size(), (long)str.data());
		});

	set_host_function(
		"Data::fill_vector",
		"void sys_data_fill_vector (rust_vec_u32_t* out)",
		[](Script& script) {
			auto [self] = script.machine().sysargs<gaddr_t>();
			auto& vec = *script.machine().memory.memarray<RustVec<uint32_t>>(self, 1);
			for (uint32_t i = 1; i <= 10; i++)
				vec.push_back(script.machine(), i);
			printf("  [Data::fill_vector] pushed %zu elements, capacity %zu\n",
				vec.size(), vec.capacity());
		});

	// --- RPC host functions ---
	//
	// The guest hands over a trampoline address plus the bytes its closure
	// captured. Both are only meaningful inside a VM running this binary,
	// which is exactly what the peer is.

	set_host_function(
		"RPC::callback",
		"void sys_rpc_callback (rpc_callback_t func, void* data, size_t size)",
		[](Script& script) {
			auto [func, data, size] =
				script.machine().sysargs<gaddr_t, gaddr_t, gaddr_t>();

			CaptureStorage capture = create_capture(script.machine(), data, size);

			script.call(func, capture);
		});

	set_host_function(
		"RPC::invoke",
		"long sys_rpc_invoke (rpc_callback_t func, void* data, size_t size)",
		[](Script& script) {
			auto [func, data, size] =
				script.machine().sysargs<gaddr_t, gaddr_t, gaddr_t>();

			if (!script.m_peer)
				throw std::runtime_error("No peer script configured for RPC");

			CaptureStorage capture = create_capture(script.machine(), data, size);

			auto& peer = *script.m_peer;
			auto result = peer.call(func, capture);

			script.machine().set_result(result.value_or(0));
		});
}
