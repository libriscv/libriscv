#pragma once
#include "types.hpp"
#include <array>
#include <string>

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

		inline void nanbox() { this->i32[1] = 0; }
		void set_float(float f) {
			this->f32[0] = f;
			this->nanbox();
		}
		void set_double(double d) {
			this->f64 = d;
		}
		void load_u32(uint32_t val) {
			this->i32[0] = val;
			this->nanbox();
		}
		void load_u64(uint64_t val) {
			this->i64 = val;
		}
	};

	template <int W>
	struct alignas(16) Registers
	{
		using address_t  = address_type<W>;   // one unsigned memory address
		using register_t = register_type<W>;  // integer register
		union FCSR {
			struct {
				uint32_t fflags : 5;
				uint32_t frm    : 3;
				uint32_t resv24 : 24;
			};
			uint32_t whole = 0;
		};

		register_t& get(uint32_t idx) { return m_reg[idx]; }
		const register_t& get(uint32_t idx) const { return m_reg[idx]; }

		fp64reg& getfl(uint32_t idx) { return m_regfl[idx]; }
		const fp64reg& getfl(uint32_t idx) const { return m_regfl[idx]; }

		register_t& at(uint32_t idx) { return m_reg.at(idx); }
		const register_t& at(uint32_t idx) const { return m_reg.at(idx); }

		FCSR& fcsr() noexcept { return m_fcsr; }

		std::string to_string() const;
		std::string flp_to_string() const;

		address_t pc = 0;
	private:
		std::array<register_t, 32> m_reg;
		std::array<fp64reg, 32> m_regfl;
		// FP control register
		FCSR m_fcsr;
	};

	static_assert(sizeof(fp64reg) == 8, "FP-register is 64-bit");
}
