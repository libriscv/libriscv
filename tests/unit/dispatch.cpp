#include <catch2/catch_test_macros.hpp>
#include <libriscv/machine.hpp>
#include <array>
using namespace riscv;
extern std::vector<uint8_t> build_and_load(const std::string&, const std::string&, bool);

// The threaded/switch dispatcher (cpu_dispatch.cpp) and the tail-call
// dispatcher (tailcall_dispatch.cpp) must agree on counters and on where
// execution resumes after a host call. These tests follow cpu_dispatch.cpp.

namespace {
struct HostState {
	unsigned calls = 0;
	uint64_t seen_counter = 0;
	uint64_t penalty = 0;
	bool stop = false;
	int64_t move_pc = 0; // ECALL: new PC (minus 4), 0 = unchanged
};

std::vector<uint8_t> build_asm(const std::string& body)
{
	std::string source = "__attribute__((naked)) void _start(void) { __asm__ volatile(\".option norvc\\n";
	for (char c : body) source += c == '\n' ? "\\n" : std::string(1, c);
	source += "\"); }";
	// The guest is hand-written assembly, so pin it to GCC: Zig rejects -march=rv64g.
	const char* cc = getenv("RVVCC");
	setenv("RCC", cc ? cc : "riscv64-linux-gnu-gcc-14", 1);
	return build_and_load(source,
		"-nostdlib -static -march=rv64g -mabi=lp64d -Wl,-e,_start", false);
}

MachineOptions<RISCV64> make_options(bool translated)
{
	MachineOptions<RISCV64> options;
	options.memory_max = 8u << 20;
	options.use_shared_execute_segments = false;
#ifdef RISCV_BINARY_TRANSLATION
	options.translate_enabled = translated;
	options.translation_cache = false;
	options.translate_enable_embedded = false;
#endif
#ifdef RISCV_ASMJIT
	options.asmjit_enabled = translated;
#endif
	(void)translated;
	return options;
}

void host_call(Machine<RISCV64>& machine)
{
	auto& state = *machine.get_userdata<HostState>();
	state.calls++;
	state.seen_counter = machine.instruction_counter();
	if (state.penalty) machine.penalize(state.penalty);
	if (state.move_pc) machine.cpu.registers().pc = state.move_pc;
	if (state.stop) machine.stop();
}

constexpr bool translated_backends =
#if defined(RISCV_BINARY_TRANSLATION) || defined(RISCV_ASMJIT)
	true;
#else
	false;
#endif
} // namespace

TEST_CASE("EBREAK handler penalties are counted", "[Dispatch]")
{
	Machine<RISCV64>::install_syscall_handler(SYSCALL_EBREAK, host_call);
	const auto binary = build_asm(
		"li a0, 0\n"
		"ebreak\n"
		"addi a0, a0, 1\n"
		".word 0x7ff00073\n");

	for (bool translated : {false, true}) {
		if (translated && !translated_backends) continue;
		CAPTURE(translated);
		const auto start = Machine<RISCV64>(binary, make_options(translated)).address_of("_start");

		// The penalty is part of the final count
		{
			Machine<RISCV64> machine(binary, make_options(translated));
			HostState state;
			state.penalty = 1000;
			machine.set_userdata(&state);
			REQUIRE(machine.simulate<false>(10'000));
			REQUIRE(state.calls == 1);
			REQUIRE(state.seen_counter == 2);
			REQUIRE(machine.instruction_counter() == 1004);
			REQUIRE(machine.cpu.reg(REG_ARG0) == 1);
		}
		// ... and takes effect right after the EBREAK
		{
			Machine<RISCV64> machine(binary, make_options(translated));
			HostState state;
			state.penalty = 1000;
			machine.set_userdata(&state);
			REQUIRE(!machine.simulate<false>(500));
			REQUIRE(machine.instruction_limit_reached());
			REQUIRE(state.calls == 1);
			REQUIRE(machine.instruction_counter() == 1002);
			REQUIRE(machine.cpu.reg(REG_ARG0) == 0);
			// bintr runs EBREAK as a system call and resumes at PC + 4
			if (!translated)
				REQUIRE(machine.cpu.pc() == start + 4);
		}
	}
}

TEST_CASE("EBREAK handler that stops leaves PC at the EBREAK", "[Dispatch]")
{
	Machine<RISCV64>::install_syscall_handler(SYSCALL_EBREAK, host_call);
	const auto binary = build_asm(
		"li a0, 0\n"
		"ebreak\n"
		"addi a0, a0, 1\n"
		".word 0x7ff00073\n");

	for (bool translated : {false, true}) {
		if (translated && !translated_backends) continue;
		CAPTURE(translated);
		Machine<RISCV64> machine(binary, make_options(translated));
		HostState state;
		state.stop = true;
		machine.set_userdata(&state);
		REQUIRE(machine.simulate<false>(10'000));
		REQUIRE(state.calls == 1);
		REQUIRE(machine.instruction_counter() == 2);
		REQUIRE(machine.cpu.reg(REG_ARG0) == 0);
		// bintr runs EBREAK as a system call and resumes at PC + 4
		if (!translated)
			REQUIRE(machine.cpu.pc() == machine.address_of("_start") + 4);
	}
}

TEST_CASE("ECALL handler that stops resumes after the ECALL", "[Dispatch]")
{
	Machine<RISCV64>::install_syscall_handler(500, host_call);
	const auto binary = build_asm(
		"li a0, 0\n"
		"li a7, 500\n"
		"ecall\n"
		"addi a0, a0, 1\n"
		".word 0x7ff00073\n");

	for (bool translated : {false, true}) {
		if (translated && !translated_backends) continue;
		CAPTURE(translated);
		Machine<RISCV64> machine(binary, make_options(translated));
		HostState state;
		state.stop = true;
		machine.set_userdata(&state);
		REQUIRE(machine.simulate<false>(10'000));
		REQUIRE(state.calls == 1);
		REQUIRE(machine.instruction_counter() == 3);
		REQUIRE(machine.cpu.reg(REG_ARG0) == 0);
		REQUIRE(machine.cpu.pc() == machine.address_of("_start") + 12);
	}
}

TEST_CASE("ECALL handler moving PC to another segment resumes at PC + 4", "[Dispatch]")
{
	Machine<RISCV64>::install_syscall_handler(500, host_call);
	const auto binary = build_asm(
		"li a0, 0\n"
		"li a7, 500\n"
		"ecall\n"
		"addi a0, a0, 1\n"
		".word 0x7ff00073\n");

	for (bool translated : {false, true}) {
		if (translated && !translated_backends) continue;
		CAPTURE(translated);
		Machine<RISCV64> machine(binary, make_options(translated));
		// Two pages: the first stays non-executable, the second holds
		// "li a0, 77; stop". The handler sets PC to the last word of the
		// first page, as a clone-like system call would (PC + 4 resumes).
		const auto area = machine.memory.mmap_allocate(2 * Page::size());
		const auto target = (area + Page::size()) & ~address_type<RISCV64>(Page::size() - 1);
		REQUIRE(target + Page::size() <= area + 2 * Page::size());
		const std::array<uint32_t, 2> code { 0x04d00513, 0x7ff00073 };
		machine.memory.memcpy(target, code.data(), sizeof(code));
		machine.memory.set_page_attr(target, Page::size(), {.read = true, .write = false, .exec = true});

		HostState state;
		state.move_pc = target - 4;
		machine.set_userdata(&state);
		REQUIRE(machine.simulate<false>(10'000));
		REQUIRE(state.calls == 1);
		REQUIRE(machine.cpu.reg(REG_ARG0) == 77);
		REQUIRE(machine.cpu.pc() == target + 8);
	}
}

TEST_CASE("Illegal instruction faults record the instruction counter", "[Dispatch]")
{
	// LOAD with funct3=7 is not a valid RV64 instruction
	const auto binary = build_asm(
		"li a0, 0\n"
		"addi a0, a0, 1\n"
		".word 0x00007503\n"
		".word 0x7ff00073\n");

	Machine<RISCV64> machine(binary, make_options(false));
	REQUIRE_THROWS_AS(machine.simulate<false>(10'000), MachineException);
	// The faulting block was counted on entry (3 or 4, depending on where
	// the block ends), and the dispatcher records it before throwing.
	REQUIRE(machine.instruction_counter() >= 3);
	REQUIRE(machine.instruction_counter() <= 4);
	REQUIRE(machine.cpu.pc() == machine.address_of("_start") + 8);
}
