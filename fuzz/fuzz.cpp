#include <libriscv/machine.hpp>
#include <climits>

static const std::vector<uint8_t> empty;
static bool init = false;

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
	static riscv::Machine<riscv::RISCV32> machine32 { empty };
	static riscv::Machine<riscv::RISCV64> machine64 { empty };
	static riscv::Machine<riscv::RISCV128> machine128 { empty };
	constexpr uint32_t S = 0x1000;
	constexpr uint32_t V = 0x2000;
	constexpr uint32_t CYCLES = 5000;

	if (UNLIKELY(len == 0 || init == false)) {
		init = true;
		machine32.memory.set_page_attr(S, 0x1000, {.read = true, .write = true});
		machine32.memory.set_page_attr(V, 0x1000, {.read = true, .write = false, .exec = true});
		machine64.memory.set_page_attr(S, 0x1000, {.read = true, .write = true});
		machine64.memory.set_page_attr(V, 0x1000, {.read = true, .write = false, .exec = true});
		machine128.memory.set_page_attr(S, 0x1000, {.read = true, .write = true});
		machine128.memory.set_page_attr(V, 0x1000, {.read = true, .write = false, .exec = true});
		return;
	}

	// Copy fuzzer data to 0x2000 and reset the stack pointer.
	machine32.cpu.reg(2) = V;
	machine32.copy_to_guest(V, data, len);
	machine32.cpu.jump(V);
#ifdef RISCV_DEBUG
	machine32.verbose_instructions = true;
	machine32.verbose_registers = true;
#endif
	try {
		// Let's avoid loops
		machine32.simulate<false>(CYCLES);
	} catch (const std::exception& e) {
		//printf(">>> Exception: %s\n", e.what());
	}

	// Again for 64-bit
	machine64.cpu.reg(2) = V;
	machine64.copy_to_guest(V, data, len);
	machine64.cpu.jump(V);
	try {
		machine64.simulate<false>(CYCLES);
	} catch (const std::exception& e) {
		//printf(">>> Exception: %s\n", e.what());
	}

	// And.. one last time for 128-bit
	machine128.cpu.reg(2) = V;
	machine128.copy_to_guest(V, data, len);
	machine128.cpu.jump(V);
	try {
		machine128.simulate<false>(CYCLES);
	} catch (const std::exception& e) {
		//printf(">>> Exception: %s\n", e.what());
	}
}

static void fuzz_elf_loader(const uint8_t* data, size_t len)
{
	const std::string_view bin {(const char*) data, len};
	try {
		riscv::Machine<riscv::RISCV32> m32 { bin };
	} catch (const std::exception& e) {
		//printf(">>> Exception: %s\n", e.what());
	}
	try {
		riscv::Machine<riscv::RISCV64> m64 { bin };
	} catch (const std::exception& e) {
		//printf(">>> Exception: %s\n", e.what());
	}
}

extern "C"
void LLVMFuzzerTestOneInput(const uint8_t* data, size_t len)
{
	//fuzz_elf_loader(data, len);
	fuzz_instruction_set(data, len);
}
