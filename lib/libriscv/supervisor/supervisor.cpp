#include "supervisor.hpp"

#include "../machine.hpp"

namespace riscv
{
	template <int W>
	bool Supervisor<W>::is_privilege_machine() const noexcept {
		return false;
	}
	template <int W>
	bool Supervisor<W>::is_privilege_supervisor() const noexcept {
		return false;
	}

	template <int W>
	void Supervisor<W>::mret()
	{
		if (!is_privilege_machine())
			throw MachineException(ILLEGAL_OPERATION, "MRET requires machine privilege level");
	}

	template <int W>
	void Supervisor<W>::sret()
	{
		if (!is_privilege_supervisor())
			throw MachineException(ILLEGAL_OPERATION, "SRET requires supervisor privilege level");

	}

	template struct Supervisor<4>;
	template struct Supervisor<8>;
}
