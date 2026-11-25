// Compare interpreter and WebAssembly translation state at each syscall.
// Instruction-limit checks can occur at different PCs in the two engines.
//
//   node difftest.js [--no-counting] program.elf [args...]
#include <libriscv/machine.hpp>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <iterator>

using Machine = riscv::Machine<riscv::RISCV64>;
using riscv::MachineOptions;
using riscv::RISCV64;

struct Record {
	uint64_t pc;
	uint64_t sysno;
	uint64_t counter;
	uint32_t regs_crc;
	uint32_t fregs_crc;
	std::array<uint64_t, 32> regs;
};
static std::vector<Record>* g_log = nullptr;
static std::string* g_output = nullptr;
static std::array<Machine::syscall_t, RISCV_SYSCALLS_MAX> g_original;

static uint32_t crc(const void* data, size_t len) {
	uint32_t h = 2166136261u; // FNV-1a
	for (size_t i = 0; i < len; i++) h = (h ^ ((const uint8_t*)data)[i]) * 16777619u;
	return h;
}

static void record(Machine& m, uint64_t sysno) {
	if (g_log == nullptr) return;
	Record r;
	r.pc = m.cpu.pc();
	r.sysno = sysno;
	r.counter = m.instruction_counter();
	for (int i = 0; i < 32; i++) r.regs[i] = m.cpu.reg(i);
	r.regs_crc = crc(r.regs.data(), sizeof(r.regs));
	r.fregs_crc = crc(&m.cpu.registers().getfl(0), 32 * sizeof(m.cpu.registers().getfl(0)));
	g_log->push_back(r);
}

template <size_t N>
static void wrapper(Machine& m) {
	record(m, N);
	g_original[N](m);
}
template <size_t N>
static void wrap_handler() {
	// Preserve the original handler when the wrapper survives a run.
	if (Machine::syscall_handlers[N] != &wrapper<N>)
		g_original[N] = Machine::syscall_handlers[N];
	Machine::syscall_handlers[N] = &wrapper<N>;
}
template <size_t... I>
static void wrap_all(std::index_sequence<I...>) {
	(wrap_handler<I>(), ...);
}

// Give both runs identical clock and random inputs.
static uint64_t g_fake_time = 0;
static uint64_t fake_now_ns() { return g_fake_time += 1'000'000; }
static void make_deterministic(Machine& machine)
{
	machine.set_rdtime([] (const Machine&) -> uint64_t { return fake_now_ns() / 1000; });
	const auto clock_gettime = [] (Machine& m) {
		const auto [clkid, buffer] = m.sysargs<int, uint64_t>();
		(void)clkid;
		const uint64_t ns = fake_now_ns();
		const int64_t ts[2] = { int64_t(ns / 1'000'000'000), int64_t(ns % 1'000'000'000) };
		m.copy_to_guest(buffer, ts, sizeof(ts));
		m.set_result(0);
	};
	Machine::install_syscall_handler(113, clock_gettime);
	Machine::install_syscall_handler(403, clock_gettime);
	Machine::install_syscall_handler(169, [] (Machine& m) { // gettimeofday
		const auto [buffer] = m.sysargs<uint64_t>();
		const uint64_t ns = fake_now_ns();
		const int64_t tv[2] = { int64_t(ns / 1'000'000'000), int64_t(ns % 1'000'000'000 / 1000) };
		if (buffer) m.copy_to_guest(buffer, tv, sizeof(tv));
		m.set_result(0);
	});
	Machine::install_syscall_handler(278, [] (Machine& m) { // getrandom
		const auto [buffer, len] = m.sysargs<uint64_t, uint64_t>();
		for (uint64_t i = 0; i < len && i < 256; i++)
			m.memory.write<uint8_t>(buffer + i, uint8_t(i * 37 + 11));
		m.set_result(std::min<uint64_t>(len, 256));
	});
	// The AT_RANDOM bytes on the initial stack: argc, argv, envp, then auxv
	uint64_t sp = machine.cpu.reg(riscv::REG_SP);
	const uint64_t argc = machine.memory.read<uint64_t>(sp);
	sp += 8 * (argc + 2);
	while (machine.memory.read<uint64_t>(sp) != 0) sp += 8;
	for (sp += 8; machine.memory.read<uint64_t>(sp) != 0; sp += 16) {
		if (machine.memory.read<uint64_t>(sp) == 25) { // AT_RANDOM
			const uint64_t addr = machine.memory.read<uint64_t>(sp + 8);
			for (int i = 0; i < 16; i++) machine.memory.write<uint8_t>(addr + i, uint8_t(0xA5 ^ i));
		}
	}
}

struct Result {
	std::vector<Record> log;
	std::string output;
	uint64_t instructions = 0;
	int exit_code = 0;
	double ms = 0.0;
	std::string error;
};

static Result run(const std::vector<uint8_t>& binary, const std::vector<std::string>& args,
	bool translate, bool counting)
{
	Result res;
	g_log = &res.log;
	const auto t0 = std::chrono::steady_clock::now();
	try {
		Machine machine { binary, MachineOptions<RISCV64>{
			.memory_max = 512ULL << 20,
			.verbose_loader = getenv("VERBOSE") != nullptr,
			.translate_enabled = translate,
			.translate_timing = getenv("TIMING") != nullptr,
			.translate_ignore_instruction_limit = !counting,
		}};
		machine.setup_linux_syscalls(false, false);
		machine.setup_posix_threads();
		machine.setup_linux(args, { "LC_ALL=C", "USER=groot" });
		g_fake_time = 0;
		make_deterministic(machine);
		g_output = &res.output;
		machine.set_printer([] (const Machine&, const char* data, size_t len) {
			g_output->append(data, len);
		});
		wrap_all(std::make_index_sequence<RISCV_SYSCALLS_MAX>{});
		if (counting)
			machine.simulate(30'000'000'000ULL);
		else
			machine.cpu.simulate_inaccurate(machine.cpu.pc());
		res.exit_code = machine.return_value<int>();
		res.instructions = machine.instruction_counter();
	} catch (const std::exception& e) {
		res.error = e.what();
	}
	res.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	g_log = nullptr;
	return res;
}

int main(int argc, char** argv)
{
	bool counting = true;
	int argi = 1;
	if (argi < argc && std::string(argv[argi]) == "--no-counting") { counting = false; argi++; }
	if (argi >= argc) {
		fprintf(stderr, "usage: %s [--no-counting] program.elf [args...]\n", argv[0]);
		return 2;
	}
	std::ifstream f(argv[argi], std::ios::binary);
	if (!f) { fprintf(stderr, "Could not open %s\n", argv[argi]); return 2; }
	const std::vector<uint8_t> binary { std::istreambuf_iterator<char>(f), {} };
	std::vector<std::string> args;
	for (int i = argi; i < argc; i++) args.push_back(argv[i]);

	const auto a = run(binary, args, false, counting);
	const auto b = run(binary, args, true, counting);

	printf("interpreter: %zu system calls, exit %d, %.1f ms%s%s\n", a.log.size(), a.exit_code, a.ms,
		a.error.empty() ? "" : ", error: ", a.error.c_str());
	printf("translated:  %zu system calls, exit %d, %.1f ms%s%s\n", b.log.size(), b.exit_code, b.ms,
		b.error.empty() ? "" : ", error: ", b.error.c_str());

	int failures = 0;
	const size_t n = std::min(a.log.size(), b.log.size());
	for (size_t i = 0; i < n && failures < 5; i++) {
		const auto& x = a.log[i];
		const auto& y = b.log[i];
		const bool counter_ok = !counting || x.counter == y.counter;
		if (x.pc != y.pc || x.sysno != y.sysno || x.regs_crc != y.regs_crc
			|| x.fregs_crc != y.fregs_crc || !counter_ok)
		{
			printf("MISMATCH at system call #%zu: pc 0x%" PRIx64 " vs 0x%" PRIx64 ", sysno %" PRIu64 " vs %" PRIu64
				", counter %" PRIu64 " vs %" PRIu64 "%s\n",
				i, x.pc, y.pc, x.sysno, y.sysno, x.counter, y.counter,
				x.fregs_crc != y.fregs_crc ? ", FP registers differ" : "");
			for (int r = 0; r < 32; r++)
				if (x.regs[r] != y.regs[r])
					printf("  x%d: 0x%" PRIx64 " vs 0x%" PRIx64 "\n", r, x.regs[r], y.regs[r]);
			failures++;
		}
	}
	if (a.log.size() != b.log.size()) { printf("MISMATCH: system call count\n"); failures++; }
	if (a.output != b.output) { printf("MISMATCH: output differs\n"); failures++; }
	if (a.exit_code != b.exit_code || a.error != b.error) { printf("MISMATCH: exit\n"); failures++; }
	if (counting && a.instructions != b.instructions) {
		printf("MISMATCH: instructions %" PRIu64 " vs %" PRIu64 "\n", a.instructions, b.instructions);
		failures++;
	}
	if (getenv("SHOW_OUTPUT")) printf("%s", b.output.c_str());
	printf("%s\n", failures ? "FAILED" : "OK");
	return failures ? 1 : 0;
}
