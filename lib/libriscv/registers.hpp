#pragma once
#include "types.hpp"
#include <array>
#include <memory>
#include <string>
#ifdef RISCV_EXT_VECTOR
#include "rvv_registers.hpp"
#endif

namespace riscv
{
	union fp64reg {
		int32_t i32[2];
		float   f32[2];
		int64_t i64 = 0;
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
	struct alignas(32) Registers
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

#ifdef RISCV_EXT_VECTOR
		auto& rvv() { return *m_rvv; }
		const auto& rvv() const { return *m_rvv; }
#endif

		Registers() {
#ifdef RISCV_EXT_VECTOR
			m_rvv.reset(new VectorRegisters<W>);
#endif
		}
		Registers(const Registers& other)
			: pc    { other.pc }, m_reg { other.m_reg }, m_fcsr { other.m_fcsr }, m_regfl { other.m_regfl }
		{
#ifdef RISCV_EXT_VECTOR
			m_rvv.reset(new VectorRegisters<W> (other.rvv()));
#endif
		}
		enum class Options { Everything, NoVectors };

		Registers& operator =(const Registers& other) {
			this->copy_from(Options::Everything, other);
			return *this;
		}
		inline void copy_from(Options opts, const Registers& other) {
			this->pc    = other.pc;
			this->m_reg = other.m_reg;
			this->m_fcsr = other.m_fcsr;
			this->m_regfl = other.m_regfl;
#ifdef RISCV_EXT_VECTOR
			if (opts == Options::Everything) {
				m_rvv.reset(new VectorRegisters<W>(other.rvv()));
			}
#endif
			(void)opts;
		}

		address_t pc = 0;
	private:
		// General purpose registers
		std::array<register_t, 32> m_reg {};
		// FP control register
		FCSR m_fcsr {};
		// General FP registers
		std::array<fp64reg, 32> m_regfl {};
#ifdef RISCV_EXT_VECTOR
		std::unique_ptr<VectorRegisters<W>> m_rvv = nullptr;
#endif
	};

	static_assert(sizeof(fp64reg) == 8, "FP-register is 64-bit");
} // riscv
