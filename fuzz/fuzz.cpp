#include <cstddef>
#include <cstdint>
#include <libriscv/machine.hpp>

extern "C"
void LLVMFuzzerTestOneInput(const uint8_t* data, size_t len)
{
	static std::vector<uint8_t> bin {};
	static riscv::Machine<riscv::RISCV32> machine { bin };

	if (UNLIKELY(len == 0)) return;

	// Copy fuzzer data to 0x1000 and skip the zero-page.
	// Non-zero length guarantees that the page will be created.
	machine.copy_to_guest(0x1000, data, len);
	machine.cpu.jump(0x1000);
	try {
		// Let's avoid loops
		machine.simulate(5000);
	} catch (std::exception& e) {
		//printf(">>> Exception: %s\n", e.what());
	}
}
