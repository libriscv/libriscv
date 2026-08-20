#include <catch2/catch_test_macros.hpp>

#include <libriscv/machine.hpp>
using namespace riscv;

static const uint64_t MAX_INSTRUCTIONS = 10'000ul;
/* Somewhere in the middle of the arena, page-aligned. */
static const uint64_t CODE_BASE = 0x100000;

/* The decoder gives up on straight-line blocks after 255 half-words and turns
 * the instruction it stopped at into a block-ending RV32I_BC_FUNCBLOCK, which
 * runs through the generic instruction handler instead of a bytecode. AUIPC is
 * the only non-block-ending instruction that reads the PC, and the generic
 * handler reads it from the register file -- which the dispatcher does not keep
 * in sync inside a block. So AUIPC has to be tested at exactly that offset. */
static uint64_t run_auipc_after(unsigned nops)
{
	std::vector<uint32_t> program;
	for (unsigned i = 0; i < nops; i++)
		program.push_back(0x00000013); // NOP (ADDI x0, x0, 0)
	program.push_back(0x00000517);     // AUIPC a0, 0
	program.push_back(0x7ff00073);     // STOP

	Machine<RISCV64> machine;
	machine.copy_to_guest(CODE_BASE, program.data(), program.size() * 4);
	if constexpr (riscv::virtual_paging_enabled) {
		machine.memory.set_page_attr(CODE_BASE, Page::size() * 2,
			{ .read = true, .write = true, .exec = true });
	}
	machine.cpu.jump(CODE_BASE);
	machine.simulate(MAX_INSTRUCTIONS);

	return machine.cpu.reg(REG_ARG0);
}

TEST_CASE("AUIPC knows its own address", "[PCrel]")
{
	/* A short block: AUIPC gets its own bytecode. */
	REQUIRE(run_auipc_after(4) == CODE_BASE + 4 * 4);

	/* The 128th instruction of the block is the one the decoder stops at. */
	REQUIRE(run_auipc_after(127) == CODE_BASE + 127 * 4);

	/* And a few instructions on either side of it, for good measure. */
	for (unsigned nops = 120; nops <= 135; nops++)
		REQUIRE(run_auipc_after(nops) == CODE_BASE + nops * 4);
}
