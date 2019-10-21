#include "cpu.hpp"

namespace riscv
{
	template <int W>
	uint32_t CPU<W>::peek32(address_t address)
	{
		return this->machine().memory.read<32>(address);
	}

	template <>
	uint32_t CPU<4>::peek32(address_t address);
	//template <>
	//uint32_t CPU<8>::peek32(address_t address);

}
