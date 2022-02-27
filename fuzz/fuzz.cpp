#include <libriscv/machine.hpp>
#include <climits>

static const std::vector<uint8_t> empty;
static bool init = false;
static const int W = RISCV_ARCH;

/* It is necessary to link with libgcc when fuzzing.
   See llvm.org/PR30643 for details. */
__attribute__((weak, no_sanitize("undefined")))
extern "C" __int128_t
__muloti4(__int128_t a, __int128_t b, int* overflow) {
	const int N = (int)(sizeof(__int128_t) * CHAR_BIT);
	const __int128_t MIN = (__int128_t)1 << (N - 1);
	const __int128_t MAX = ~MIN;
	*overflow = 0;
	__int128_t result = a * b;
	if (a == MIN) {
	if (b != 0 && b != 1)
	  *overflow = 1;
	return result;
	}
	if (b == MIN) {
	if (a != 0 && a != 1)
	  *overflow = 1;
	return result;
	}
	__int128_t sa = a >> (N - 1);
	__int128_t abs_a = (a ^ sa) - sa;
	__int128_t sb = b >> (N - 1);
	__int128_t abs_b = (b ^ sb) - sb;
	if (abs_a < 2 || abs_b < 2)
	return result;
	if (sa == sb) {
	if (abs_a > MAX / abs_b)
	  *overflow = 1;
	} else {
	if (abs_a > MIN / -abs_b)
	  *overflow = 1;
	}
	return result;
}

static void fuzz_instruction_set(const uint8_t* data, size_t len)
{
	static riscv::Machine<W> machine { empty };
	constexpr uint32_t S = 0x1000;
	constexpr uint32_t V = 0x2000;
	constexpr uint32_t CYCLES = 5000;

	if (UNLIKELY(len == 0 || init == false)) {
		init = true;
		machine.memory.set_page_attr(S, 0x1000, {.read = true, .write = true});
		machine.memory.set_page_attr(V, 0x1000, {.read = true, .write = false, .exec = true});
		return;
	}

	// Copy fuzzer data to 0x2000 and reset the stack pointer.
	machine.cpu.reg(riscv::REG_SP) = V;
	machine.copy_to_guest(V, data, len);
	machine.cpu.jump(V);
#ifdef RISCV_DEBUG
	machine.verbose_instructions = true;
	machine.verbose_registers = true;
#endif
	try {
		// Let's avoid loops
		machine.simulate<false>(CYCLES);
	} catch (const std::exception& e) {
		//printf(">>> Exception: %s\n", e.what());
	}
}

static void fuzz_elf_loader(const uint8_t* data, size_t len)
{
	using namespace riscv;
	const std::string_view bin {(const char*) data, len};
	try {
		const MachineOptions<W> options { .allow_write_exec_segment = true };
		Machine<W> m32 { bin, options };
	} catch (const std::exception& e) {
		//printf(">>> Exception: %s\n", e.what());
	}
}

extern "C"
void LLVMFuzzerTestOneInput(const uint8_t* data, size_t len)
{
#if defined(FUZZ_ELF)
	fuzz_elf_loader(data, len);
#elif defined(FUZZ_VM)
	fuzz_instruction_set(data, len);
#else
	#error "Unknown fuzzing mode"
#endif
}
