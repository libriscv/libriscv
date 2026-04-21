#include "script.hpp"
#include <time.h>
using gaddr_t = Script::gaddr_t;
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
		"int sys_math_add (int, int)",
		[](Script& script) {
			auto [a, b] = script.machine().sysargs<int, int>();
			printf("  [Math::add] %d + %d = %d\n", a, b, a + b);
			script.machine().set_result(a + b);
		});

	set_host_function(
		"Math::multiply",
		"int sys_math_multiply (int, int)",
		[](Script& script) {
			auto [a, b] = script.machine().sysargs<int, int>();
			printf("  [Math::multiply] %d * %d = %d\n", a, b, a * b);
			script.machine().set_result(a * b);
		});

	set_host_function(
		"IO::print",
		"void sys_io_print (const char*)",
		[](Script& script) {
			auto [msg] = script.machine().sysargs<std::string>();
			printf("  [IO::print] %s\n", msg.c_str());
		});

	set_host_function(
		"Game::get_time",
		"double sys_game_get_time ()",
		[](Script& script) {
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			double t = ts.tv_sec + ts.tv_nsec * 1e-9;
			script.machine().set_result(t);
		});

	set_host_function(
		"Game::init_world",
		"void sys_game_init_world (const char*)",
		[](Script& script) {
			auto [world_name] = script.machine().sysargs<std::string>();
			printf("  [Game::init_world] Initializing world '%s'\n", world_name.c_str());
		});

	// --- RPC host functions ---

	set_host_function(
		"RPC::callback",
		"void sys_rpc_callback (rpc_callback_t, void*, size_t)",
		[](Script& script) {
			auto [func, data, size] =
				script.machine().sysargs<gaddr_t, gaddr_t, gaddr_t>();

			CaptureStorage capture = create_capture(script.machine(), data, size);

			script.call(func, capture);
		});

	set_host_function(
		"RPC::invoke",
		"long sys_rpc_invoke (rpc_callback_t, void*, size_t)",
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
