#pragma once
#include "cpu.hpp"
#include "memory.hpp"
#include <vector>

namespace riscv
{
	static constexpr int RISCV32 = 4;
	static constexpr int RISCV64 = 8;

	template <int W>
	struct Machine
	{
		Machine(std::vector<uint8_t> binary);

		bool stopped() const noexcept;
		void simulate();

		CPU<W>    cpu;
		Memory<W> memory;


		static_assert((W == 4 || W == 8), "Must be either 4-byte or 8-byte ISA");
	};

#include "machine_inline.hpp"
}
