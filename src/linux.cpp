#include "linux.hpp"
#include "auxvec.hpp"
#include <sys/random.h>
#include <array>
using namespace riscv;

static inline
void push_arg(Machine<4>& m, std::vector<uint32_t>& vec, uint32_t& dst, const std::string& str)
{
	dst -= str.size();
	dst &= ~0x3; // maintain alignment
	vec.push_back(dst);
	m.memory.copy_to_guest(dst, (const uint8_t*) str.data(), str.size());
}
static inline
void push_aux(Machine<4>& m, std::vector<uint32_t>& vec, AuxVec<uint32_t> aux)
{
	vec.push_back(aux.a_type);
	vec.push_back(aux.a_val);
}
static inline
void push_down(Machine<4>& m, uint32_t& dst, const uint8_t* data, size_t size)
{
	dst -= size;
	dst &= ~0x3; // maintain alignment
	m.memory.copy_to_guest(dst, (const uint8_t*) data, size);
}

template <>
void prepare_linux(riscv::Machine<4>& machine, const std::vector<std::string>& args)
{
	// Build AUX-vector for C-runtime
	using auxv_t = AuxVec<uint32_t>;

	// start installing at near-end of address space, leaving room on both sides
	// stack below and installation above
	uint32_t dst = machine.memory.stack_initial();

	// inception :)
	const uint32_t canary_addr = dst;
	std::array<uint8_t, 16> canary;
	getrandom(canary.data(), canary.size(), GRND_RANDOM);
	push_down(machine, dst, canary.data(), canary.size());

	const uint32_t platform_addr = dst;
	const std::string platform = "RISC-V RV32imc";
	push_down(machine, dst, (const uint8_t*) platform.data(), platform.size());

	// Parameters to main
	std::vector<uint32_t> argv;
	argv.push_back(args.size()); // argc
	for (const auto& string : args) {
		push_arg(machine, argv, dst, string);
	}
	argv.push_back(0x0); // last parameter

	// Env vars
	push_arg(machine, argv, dst, "LC_CTYPE=C");
	push_arg(machine, argv, dst, "LC_ALL=C");
	push_arg(machine, argv, dst, "USER=root");
	argv.push_back(0x0); // last parameter

	// auxiliary vector
	if (machine.verbose_machine) {
		printf("* Initializing aux-vector\n");
	}

	push_aux(machine, argv, {AT_PAGESZ, 4096});
	push_aux(machine, argv, {AT_CLKTCK, 100});

	// ELF related
	push_aux(machine, argv, {AT_PHENT, 0});
	push_aux(machine, argv, {AT_PHDR, 0});
	push_aux(machine, argv, {AT_PHNUM, 0});

	// Misc
	push_aux(machine, argv, {AT_BASE, 0});
	push_aux(machine, argv, {AT_FLAGS, 0});
	push_aux(machine, argv, {AT_ENTRY, machine.memory.start_address()});
	push_aux(machine, argv, {AT_HWCAP, 0});
	push_aux(machine, argv, {AT_UID, 0});
	push_aux(machine, argv, {AT_EUID, 0});
	push_aux(machine, argv, {AT_GID, 0});
	push_aux(machine, argv, {AT_EGID, 0});
	push_aux(machine, argv, {AT_SECURE, 1}); // indeed ;)

	push_aux(machine, argv, {AT_PLATFORM, platform_addr});

	// supplemental randomness
	push_aux(machine, argv, {AT_RANDOM, canary_addr});
	push_aux(machine, argv, {AT_NULL, 0});

	// from this point on the stack is starting, pointing @ argc
	// install the arg vector
	const size_t argsize = argv.size() * sizeof(argv[0]);
	dst -= argsize;
	machine.memory.copy_to_guest(dst, (const uint8_t*) argv.data(), argv.size());
	// re-initialize machine stack-pointer
	machine.cpu.reg(RISCV::REG_SP) = dst;
	if (machine.verbose_machine) {
		printf("* SP = 0x%X  Argument list: %zu bytes\n", dst, argsize);
	}
}
