#include "types.hpp"
#include <set>

namespace riscv
{
	template <int W>
	struct TransInfo
	{
		address_type<W> basepc;
		address_type<W> gp;
		int len;
		bool has_branch;
		bool forward_jumps;
		std::set<address_type<W>> jump_locations;
	};

	template <int W>
	struct TransInstr;
}
