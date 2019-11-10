#pragma once
#include "types.hpp"
#include "riscvbase.hpp"
#include <array>
#include <string>
#include <cstdio> // snprintf

namespace riscv
{
	union fp64reg {
		float  f32[2];
		double f64;
	};

	template <int W>
	struct Registers
	{
		using address_t = address_type<W>;          // one unsigned memory address
		using isa_t     = isa_type<W>;              // 32- or 64-bit architecture
		using format_t  = typename isa_t::format_t; // one machine instruction

		auto& get(uint32_t idx) { return m_reg[idx]; }
		const auto& get(uint32_t idx) const { return m_reg[idx]; }

		auto& getfl(uint32_t idx) { return m_regfl[idx]; }
		const auto& getfl(uint32_t idx) const { return m_regfl[idx]; }

		auto& at(uint32_t idx) { return m_reg.at(idx); }
		const auto& at(uint32_t idx) const { return m_reg.at(idx); }

		auto& fcsr() noexcept { return m_fcsr; }

		std::string to_string() const
		{
			char buffer[600];
			int  len = 0;
			len += snprintf(buffer+len, sizeof(buffer)-len,
							"[%s\t%8zu] ", "INSTR", (size_t) this->counter);
			for (int i = 1; i < 32; i++) {
				len += snprintf(buffer+len, sizeof(buffer) - len,
						"[%s\t%08X] ", RISCV::regname(i), this->get(i));
				if (i % 5 == 4) {
					len += snprintf(buffer+len, sizeof(buffer)-len, "\n");
				}
			}
			return std::string(buffer, len);
		}

		uint64_t  counter = 0;
		address_t pc = 0;
	private:
		std::array<typename isa_t::register_t, 32> m_reg;
		std::array<fp64reg, 32> m_regfl;
		// FP control register
		union {
			struct {
				uint32_t fflags : 5;
				uint32_t frm    : 3;
				uint32_t resv24 : 24;
			};
			uint32_t whole = 0;
		} m_fcsr;
	};
}
