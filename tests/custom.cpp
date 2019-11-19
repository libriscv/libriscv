#include <libriscv/machine.hpp>
#include <cassert>

void test_custom_machine()
{
	// this is a custom machine with very little virtual memory
	const uint32_t m2_memory = 65536;
	riscv::Machine<riscv::RISCV32> m2 { {}, m2_memory };

	// free the zero-page to reclaim 4k
	m2.memory.free_pages(0x0, riscv::Page::size());

	// fake a start at 0x1068
	const uint32_t entry_point = 0x1068;
	m2.cpu.jump(entry_point);

	assert(m2.cpu.registers().counter == 0);
	assert(m2.cpu.registers().pc == entry_point);
	assert(m2.free_memory() == 65536);
}
