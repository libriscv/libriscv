#include <libriscv/machine.hpp>
#include <cassert>
using namespace riscv;

#define assert_mem(m, type, addr, value) \
		assert(m.memory.template read<type> (addr) == value)

static const std::vector<uint32_t> instructions =
{
	0x00065637, // lui     a2,0x65
	0x000655b7, // lui     a1,0x65
	0x11612023, // sw      s6,256(sp)
	0x0b410b13, // addi    s6,sp,180
};

void test_rv32i()
{
	const uint32_t memory = 65536;
	riscv::Machine<riscv::RISCV32> m { std::string_view{}, memory };
	// install instructions
	const size_t bytes = sizeof(instructions[0]) * instructions.size();
	m.copy_to_guest(0x1000, instructions.data(), bytes);
	// make the instructions readable & executable
	m.memory.set_page_attr(0x1000, bytes, {
		 .read = true, .write = false, .exec = true
	});
	m.cpu.jump(0x1000);

	// stack frame
	m.cpu.reg(RISCV::REG_SP) = 0x120000 - 288;
	const uint32_t current_sp = m.cpu.reg(RISCV::REG_SP);

	// execute LUI a2, 0x65000
	m.simulate(1);
	assert(m.cpu.reg(RISCV::REG_ARG2) == 0x65000);
	// execute LUI a1, 0x65000
	m.simulate(1);
	assert(m.cpu.reg(RISCV::REG_ARG1) == 0x65000);
	// execute SW  s6, [SP + 256]
	m.cpu.reg(22) = 0x12345678;
	m.simulate(1);
	assert_mem(m, uint32_t, current_sp + 256, m.cpu.reg(22));
	// execute ADDI s6, [SP + 180]
	m.cpu.reg(22) = 0x0;
	m.simulate(1);
	assert(m.cpu.reg(22) == current_sp + 180);
}
