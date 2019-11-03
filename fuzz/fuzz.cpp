#include <cstddef>
#include <cstdint>
#include <libriscv/machine.hpp>

extern "C"
void LLVMFuzzerTestOneInput(const uint8_t* data, size_t len)
{
	std::vector<uint8_t> binary {};
	riscv::Machine<riscv::RISCV32> machine { binary };
	// copy fuzzer data to 0x1000 and skip the zero-page
	machine.copy_to_guest(0x1000, data, len);
	machine.cpu.registers().pc = 0x1000;
	try {
		while (!machine.stopped()) {
			machine.simulate();
			// let's avoid loops
			if (machine.cpu.registers().counter > 1000) break;
		}
	} catch (std::exception& e) {
		//printf(">>> Exception: %s\n", e.what());
	}
}
