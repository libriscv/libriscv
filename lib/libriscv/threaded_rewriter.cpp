#include "machine.hpp"
#include "instruction_list.hpp"
#include "rv32i_instr.hpp"
#include "threaded_bytecodes.hpp"

namespace riscv
{
	template <int W> RISCV_INTERNAL
	size_t CPU<W>::threaded_rewrite(size_t bytecode, address_t pc, rv32i_instruction& instr)
	{
		const auto original = instr;
		(void) pc;

		switch (bytecode)
		{
			case RV32I_BC_ADDI: {
				FasterItype rewritten;
				rewritten.rs1 = original.Itype.rd;
				rewritten.rs2 = original.Itype.rs1;
				rewritten.imm = original.Itype.signed_imm();

				instr.whole = rewritten.whole;
				return bytecode;
				}
			case RV32I_BC_BNE: {
				FasterItype rewritten;
				rewritten.rs1 = original.Btype.rs1;
				rewritten.rs2 = original.Btype.rs2;
				rewritten.imm = original.Btype.signed_imm();

				instr.whole = rewritten.whole;
				return bytecode;
				}
			case RV32I_BC_OP_ADD: {
				FasterOpType rewritten;
				rewritten.rd = original.Rtype.rd;
				rewritten.rs1 = original.Rtype.rs1;
				rewritten.rs2 = original.Rtype.rs2;

				instr.whole = rewritten.whole;
				return bytecode;
				}
			default: {

			}
		}

		return bytecode;
	}

	template struct CPU<4>;
	template struct CPU<8>;
	template struct CPU<16>;
} // riscv