#pragma once
#include "types.hpp"
#include <array>
#include <cstdint>

namespace riscv
{
	union alignas(RISCV_EXT_VECTOR) VectorLane {
		static constexpr unsigned VSIZE = RISCV_EXT_VECTOR;
		static constexpr unsigned size() noexcept { return VSIZE; }

		std::array<uint8_t,  VSIZE / 1> u8 = {};
		std::array<uint16_t, VSIZE / 2> u16;
		std::array<uint32_t, VSIZE / 4> u32;
		std::array<uint64_t, VSIZE / 8> u64;

		std::array<float,  VSIZE / 4> f32;
		std::array<double, VSIZE / 8> f64;

		// Typed element access. T may be a signed or unsigned integer
		// or a floating-point type of 1, 2, 4 or 8 bytes.
		template <typename T>
		T& elem(const unsigned idx) noexcept {
			if constexpr (sizeof(T) == 1)
				return reinterpret_cast<T&>(u8[idx]);
			else if constexpr (sizeof(T) == 2)
				return reinterpret_cast<T&>(u16[idx]);
			else if constexpr (sizeof(T) == 4)
				return reinterpret_cast<T&>(u32[idx]);
			else if constexpr (sizeof(T) == 8)
				return reinterpret_cast<T&>(u64[idx]);
		}
		template <typename T>
		const T& elem(const unsigned idx) const noexcept {
			return const_cast<VectorLane*>(this)->elem<T>(idx);
		}

		// A lane used as a mask register holds VSIZE*8 bits.
		bool mask(const unsigned i) const noexcept {
			return (u8[i / 8] >> (i % 8)) & 1;
		}
		void set_mask(const unsigned i, const bool value) noexcept {
			u8[i / 8] = value
				? u8[i / 8] |  (uint8_t(1) << (i % 8))
				: u8[i / 8] & ~(uint8_t(1) << (i % 8));
		}
	};
	static_assert(sizeof(VectorLane) == RISCV_EXT_VECTOR, "Vectors are 32 bytes");
	static_assert(alignof(VectorLane) == RISCV_EXT_VECTOR, "Vectors are 32-byte aligned");

	template <int W>
	struct alignas(RISCV_EXT_VECTOR) VectorRegisters
	{
		using address_t  = address_type<W>;   // one unsigned memory address
		using register_t = register_type<W>;  // integer register

		auto& get(unsigned idx) noexcept { return m_vec[idx & 31]; }
		const auto& get(unsigned idx) const noexcept { return m_vec[idx & 31]; }

		register_t vl() const noexcept { return m_vl; }
		void set_vl(register_t value) noexcept { m_vl = value; }

		bool vill() const noexcept { return m_vill; }
		// Current SEW in bits (8, 16, 32 or 64).
		unsigned sew() const noexcept { return 8u << m_vsew; }
		// log2(LMUL). Negative values are fractional LMULs.
		int lmul_shift() const noexcept { return m_lmul; }
		// Number of registers in a vector register group (0 when fractional).
		unsigned group_regs() const noexcept {
			return (m_lmul >= 0) ? 1u << m_lmul : 0u;
		}
		bool vta() const noexcept { return m_vta; }
		bool vma() const noexcept { return m_vma; }

		// VLMAX = LMUL * VLEN / SEW, at least 1.
		uint64_t vlmax() const noexcept {
			// Elements per register group = VLEN/SEW * LMUL, at least 1.
			uint64_t vlmax = uint64_t(VectorLane::VSIZE) >> m_vsew;
			if (m_lmul >= 0)
				vlmax <<= m_lmul;
			else
				vlmax >>= -m_lmul;
			return vlmax ? vlmax : 1;
		}

		// Set vtype from the vtypei encoding bits. Returns false if the
		// encoding is reserved or unsupported, in which case vill is set.
		bool set_vtype(uint32_t vtypei) noexcept {
			m_vill = false;
			const uint32_t vlmul = vtypei & 0x7;
			const uint32_t vsew  = (vtypei >> 3) & 0x7;
			m_vta = vtypei & 0x40;
			m_vma = vtypei & 0x80;
			// Bits above vma/vta are reserved, as is vlmul==100.
			// SEW is limited to the widths this emulator implements.
			if (vtypei >> 8 || vlmul == 0b100 || vsew > 0b011) {
				m_vill = true;
			} else {
				m_vsew = vsew;
				m_lmul = lmul_shift_for(vlmul);
			}
			return !m_vill;
		}

	private:
		static int lmul_shift_for(uint32_t vlmul) noexcept {
			switch (vlmul) {
				case 0b000: return 0;   // LMUL=1
				case 0b001: return 1;   // LMUL=2
				case 0b010: return 2;   // LMUL=4
				case 0b011: return 3;   // LMUL=8
				case 0b101: return -3;  // LMUL=1/8
				case 0b110: return -2;  // LMUL=1/4
			}
			return -1;                  // LMUL=1/2
		}

		std::array<VectorLane, 32> m_vec {};
		register_t m_vl = 0;
		uint32_t m_vsew = 0;   // encoded SEW (0=8b .. 3=64b)
		int      m_lmul = 0;   // log2(LMUL)
		bool m_vta = false;
		bool m_vma = false;
		bool m_vill = true;    // invalid until first vsetvli
	};
}
