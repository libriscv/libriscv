#pragma once
#include "types.hpp"
#include "riscvbase.hpp"
#include <array>
#include <string>
#include <cstdio> // snprintf

namespace riscv
{
	union fp64reg {
		int32_t i32[2];
		float   f32[2];
		int64_t i64;
		double  f64;
		struct {
			uint32_t bits  : 31;
			uint32_t sign  : 1;
			uint32_t upper;
		} lsign;
		struct {
			uint64_t bits  : 63;
			uint64_t sign  : 1;
		} usign;

		inline void nanbox() { this->i32[1] = 0xFFFFFFFF; }
		void load_u32(uint32_t val) {
			this->i32[0] = val;
			this->i32[1] = -1;
		}
		void load_u64(uint64_t val) {
			this->i64 = val;
		}
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

		std::string flp_to_string() const
		{
			char buffer[800];
			int  len = 0;
			for (int i = 0; i < 32; i++) {
				auto& src = this->getfl(i);
				const char T = (src.i32[1] == -1) ? 'S' : 'D';
				double val = (src.i32[1] == -1) ? src.f32[0] : src.f64;
				len += snprintf(buffer+len, sizeof(buffer) - len,
						"[%s\t%c%+.2f (0x%lX)] ", RISCV::flpname(i), T, val, src.i64);
				if (i % 4 == 3) {
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

	static_assert(sizeof(fp64reg) == 8, "FP-register is 64-bit");
}
