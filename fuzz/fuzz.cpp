#include <cstddef>
#include <cstdint>
#include <libriscv/machine.hpp>

static std::vector<uint8_t> binary {};
// we don't want to see unhandled syscall messages
namespace riscv {
	bool verbose_machine = false;
}


extern "C"
void LLVMFuzzerTestOneInput(const uint8_t* data, size_t len)
{
	riscv::Machine<riscv::RISCV32> machine { binary };
	// copy fuzzer data to 0x1000 and skip the zero-page
	machine.copy_to_guest(0x1000, data, len);
	machine.cpu.registers().pc = 0x1000;
	try {
		// let's avoid loops
		machine.simulate(5000);
	} catch (std::exception& e) {
		//printf(">>> Exception: %s\n", e.what());
	}
}
