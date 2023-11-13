#include "types.hpp"
#include "rv32i_instr.hpp"
#include <set>

namespace riscv
{
	template <int W>
	struct TransInstr;

	template <int W>
	struct TransInfo
	{
		const rv32i_instruction* instr;
		address_type<W> basepc;
		address_type<W> endpc;
		address_type<W> gp;
		int len;
		bool forward_jumps;
		std::set<address_type<W>> jump_locations;
	};
}
