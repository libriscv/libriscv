#include <cstddef>
#include <cstdint>
#include <libriscv/machine.hpp>

static const std::vector<uint8_t> empty {};

static void fuzz_instruction_set(const uint8_t* data, size_t len)
{
	static riscv::Machine<riscv::RISCV32> machine32 { empty };
	static riscv::Machine<riscv::RISCV64> machine64 { empty };

	if (UNLIKELY(len == 0)) return;

	// Copy fuzzer data to 0x1000 and skip the zero-page.
	// Non-zero length guarantees that the page will be created.
	machine32.copy_to_guest(0x1000, data, len);
	machine32.cpu.jump(0x1000);
	try {
		// Let's avoid loops
		machine32.simulate(5000);
	} catch (const std::exception& e) {
		//printf(">>> Exception: %s\n", e.what());
	}
	// Again for 64-bit
	machine64.copy_to_guest(0x1000, data, len);
	machine64.cpu.jump(0x1000);
	try {
		machine64.simulate(5000);
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
	fuzz_elf_loader(data, len);
	fuzz_instruction_set(data, len);
}
