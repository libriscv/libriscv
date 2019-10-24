#pragma once
#include "types.hpp"
#include "riscvbase.hpp"
#include <array>
#include <string>
#include <cstdio> // snprintf

namespace riscv
{
	template <int W>
	struct Registers
	{
		using address_t = address_type<W>;          // one unsigned memory address
		using isa_t     = isa_type<W>;              // 32- or 64-bit architecture
		using format_t  = typename isa_t::format_t; // one machine instruction

		auto& get(uint16_t idx) { return reg.at(idx); }
		const auto& get(uint16_t idx) const { return reg.at(idx); }

		std::string to_string() const
		{
			char buffer[600];
			int  len = 0;
			for (int i = 0; i < 32; i++) {
				len += snprintf(buffer+len, sizeof(buffer) - len,
						"[%s\t%08X] ", RISCV::regname(i), this->get(i));
				if (i % 5 == 4) {
					len += snprintf(buffer+len, sizeof(buffer)-len, "\n");
				}
			}
			len += snprintf(buffer+len, sizeof(buffer)-len,
							"[%s\t%08X] ", "PC", this->pc);
			return std::string(buffer, len);
		}

		address_t pc = 0;
		std::array<typename isa_t::register_t, 32> reg;
	};
}
