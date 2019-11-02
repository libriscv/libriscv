#include <cstddef>
#include <cstdint>
#include <libriscv/machine.hpp>

extern "C"
void LLVMFuzzerTestOneInput(const uint8_t* data, size_t len)
{
	const auto binary = std::vector<uint8_t> (data, data + len);
	try {
		riscv::Machine<riscv::RISCV32> machine { binary };
		printf(">>> Machine loaded\n");
		while (!machine.stopped()) {
			machine.simulate();
		}
	} catch (std::exception& e) {
		//printf(">>> Exception: %s\n", e.what());
	}
}
