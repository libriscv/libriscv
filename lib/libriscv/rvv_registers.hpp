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
		// ... and as vtype encodes it: log2(SEW / 8).
		unsigned encoded_sew() const noexcept { return m_vsew; }
		// log2(LMUL). Negative values are fractional LMULs.
		int lmul_shift() const noexcept { return m_lmul; }
		// Number of registers in a vector register group (0 when fractional).
		unsigned group_regs() const noexcept {
			return (m_lmul >= 0) ? 1u << m_lmul : 0u;
		}
		bool vta() const noexcept { return m_vta; }
		bool vma() const noexcept { return m_vma; }

		// VLEN in bytes, as the vlenb CSR reports it.
		static constexpr unsigned vlenb() noexcept { return VectorLane::VSIZE; }

		// The vtype CSR: the encoding vsetvl was given, with vill in the
		// top bit. An illegal vtype reads back as vill alone.
		register_t vtype() const noexcept {
			constexpr register_t vill_bit = register_t(1) << (8 * sizeof(register_t) - 1);
			return m_vill ? vill_bit : m_vtype;
		}

		// Fixed-point rounding mode (vxrm) and the saturation flag (vxsat),
		// both also reachable through vcsr.
		unsigned vxrm() const noexcept { return m_vxrm; }
		void set_vxrm(unsigned value) noexcept { m_vxrm = value & 3; }
		bool vxsat() const noexcept { return m_vxsat; }
		void set_vxsat(bool value) noexcept { m_vxsat = value; }
		// vcsr packs vxsat in bit 0 and vxrm in bits 2:1.
		unsigned vcsr() const noexcept { return (m_vxrm << 1) | (m_vxsat ? 1u : 0u); }
		void set_vcsr(unsigned value) noexcept {
			m_vxsat = value & 1;
			m_vxrm  = (value >> 1) & 3;
		}
		// vstart is written by trapping implementations only; this one always
		// completes an instruction, so it reads back as zero.
		register_t vstart() const noexcept { return m_vstart; }
		void set_vstart(register_t value) noexcept { m_vstart = value; }

		// VLMAX = LMUL * VLEN / SEW, at least 1. Every vsetvl computes it, so
		// it is spelled as one shift: log2(VLMAX) is log2(VLEN/SEW) plus
		// log2(LMUL), and a fractional LMUL that shifts it below one element
		// clamps rather than branching.
		uint64_t vlmax() const noexcept {
			constexpr int vlen_log2 = bits_of(VectorLane::VSIZE);
			const int shift = vlen_log2 - int(m_vsew) + m_lmul;
			return shift > 0 ? (uint64_t(1) << shift) : 1;
		}

		// Set vtype from the vtypei encoding bits. Returns false if the
		// encoding is reserved or unsupported, in which case vill is set.
		bool set_vtype(uint32_t vtypei) noexcept {
			const uint32_t vlmul = vtypei & 0x7;
			const uint32_t vsew  = (vtypei >> 3) & 0x7;
			m_vta = vtypei & 0x40;
			m_vma = vtypei & 0x80;
			// Bits above vma/vta are reserved, as is vlmul==100.
			// SEW is limited to the widths this emulator implements.
			m_vill = (vtypei >> 8) || vlmul == 0b100 || vsew > 0b011;
			if (LIKELY(!m_vill)) {
				m_vsew = vsew;
				m_lmul = lmul_shift_for(vlmul);
				m_vtype = vtypei;
			}
			return !m_vill;
		}

		// Fields a code generator reads directly. The asmjit backend computes
		// displacements from a live instance rather than with offsetof(), as it
		// does for the integer register file, so they survive layout changes.
		const register_t& vl_ref() const noexcept { return m_vl; }
		const uint32_t& encoded_sew_ref() const noexcept { return m_vsew; }
		const int& lmul_shift_ref() const noexcept { return m_lmul; }
		const bool& vill_ref() const noexcept { return m_vill; }

		// Verifies the C mirror in tr_api.cpp against the offsets below.
		friend struct VectorLayoutProbe;

	private:
		// log2 of a power of two.
		static constexpr int bits_of(unsigned value) noexcept {
			int bits = 0;
			while (value > 1) { value >>= 1; bits++; }
			return bits;
		}
		// vlmul is log2(LMUL) as a three-bit signed field: 000..011 are LMUL
		// 1 to 8, 101..111 are the fractional 1/8 to 1/2
		static int lmul_shift_for(uint32_t vlmul) noexcept {
			return int(vlmul) - int((vlmul & 0b100) << 1);
		}

		std::array<VectorLane, 32> m_vec {};
		register_t m_vl = 0;
		register_t m_vstart = 0;
		uint32_t m_vsew = 0;   // encoded SEW (0=8b .. 3=64b)
		uint32_t m_vtype = 0;  // the raw encoding, for the vtype CSR
		int      m_lmul = 0;   // log2(LMUL)
		uint8_t  m_vxrm = 0;   // fixed-point rounding mode
		bool m_vxsat = false;  // fixed-point saturation flag
		bool m_vta = false;
		bool m_vma = false;
		bool m_vill = true;    // invalid until first vsetvli
	};
}
