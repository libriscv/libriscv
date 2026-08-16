#include "rvv.hpp"
#include "instr_helpers.hpp"
#include <cmath>
#include <cstdint>
#include <type_traits>

namespace riscv
{
	/* RVV 1.0. VLEN is RISCV_EXT_VECTOR*8 bits, SEW 8/16/32/64, LMUL
	 * including fractional. Tail and masked-off destination elements
	 * are left unchanged (undisturbed). vstart is always zero.
	 */

namespace
{
	constexpr unsigned bits_of(const unsigned bytes) noexcept
	{
		return bytes == 1 ? 0 : bytes == 2 ? 1 : bytes == 4 ? 2 : 3;
	}

	// Load/store width field to EEW log2. 4 for invalid encodings.
	constexpr unsigned bits_lookup(const unsigned width_field) noexcept
	{
		switch (width_field) {
			case 0b000: return 0; // EEW=8
			case 0b101: return 1; // EEW=16
			case 0b110: return 2; // EEW=32
			case 0b111: return 3; // EEW=64
		}
		return 4; // invalid
	}

	template <typename CPU_t>
	RISCV_ALWAYS_INLINE void require_valid_vtype(CPU_t& cpu)
	{
		if (UNLIKELY(cpu.registers().rvv().vill()))
			cpu.trigger_exception(ILLEGAL_OPCODE);
	}

	// A register group operand must be aligned and in bounds when LMUL > 1.
	template <typename CPU_t>
	RISCV_ALWAYS_INLINE void check_register_group(CPU_t& cpu, const unsigned vreg)
	{
		const unsigned regs = cpu.registers().rvv().group_regs();
		if (UNLIKELY(regs > 1 && ((vreg % regs) != 0 || vreg + regs > 32)))
			cpu.trigger_exception(ILLEGAL_OPCODE);
	}

	// Element *i* of the register group starting at *vreg*. T is the
	// SEW-sized element type.
	template <typename T, typename RVV_t>
	RISCV_ALWAYS_INLINE T& element_at(RVV_t& rvv, const unsigned vreg, const uint64_t i)
	{
		constexpr unsigned per_reg = VectorLane::size() / sizeof(T);
		return rvv.get(vreg + i / per_reg).template elem<T>(i % per_reg);
	}

	template <typename RVV_t>
	RISCV_ALWAYS_INLINE bool element_active(const RVV_t& rvv, const bool vm, const uint64_t i)
	{
		return vm || rvv.get(0).mask(i);
	}

	// Dispatch a lambda over the current SEW as a signed integer type.
	template <typename CPU_t, typename F>
	static void int_sew_dispatch(CPU_t& cpu, F&& body)
	{
		switch (cpu.registers().rvv().sew()) {
			case 8:  body(int8_t{});  break;
			case 16: body(int16_t{}); break;
			case 32: body(int32_t{}); break;
			case 64: body(int64_t{}); break;
			default: cpu.trigger_exception(ILLEGAL_OPCODE);
		}
	}

	// Dispatch a lambda over the current SEW as a floating-point type.
	template <typename CPU_t, typename F>
	static void fp_sew_dispatch(CPU_t& cpu, F&& body)
	{
		switch (cpu.registers().rvv().sew()) {
			case 32: body(float{});  break;
			case 64: body(double{}); break;
			default: cpu.trigger_exception(ILLEGAL_OPCODE);
		}
	}

	// Masked element-wise write into vd. The lambda returns the new value
	// of element *i* and must read its sources for *i* first (vd may alias
	// vs1/vs2).
	template <typename T, typename CPU_t, typename F>
	static void vector_element_loop(CPU_t& cpu, const unsigned vd, const bool vm, F&& compute)
	{
		auto& rvv = cpu.registers().rvv();
		const uint64_t vl = rvv.vl();
		for (uint64_t i = 0; i < vl; i++) {
			if (element_active(rvv, vm, i)) {
				element_at<T>(rvv, vd, i) = compute(i);
			}
		}
	}

	// Mask-producing loop (compares, carry-out).
	template <typename CPU_t, typename F>
	static void mask_dest_loop(CPU_t& cpu, const unsigned vd, const bool vm, F&& compare)
	{
		auto& rvv = cpu.registers().rvv();
		const uint64_t vl = rvv.vl();
		auto& dest = rvv.get(vd);
		for (uint64_t i = 0; i < vl; i++) {
			if (element_active(rvv, vm, i)) {
				dest.set_mask(i, compare(i));
			}
		}
	}

	// Reduction: vd[0] = fold(vs1[0], vs2[*]); vl == 0 writes nothing.
	template <typename T, typename CPU_t, typename F>
	static void reduction_loop(CPU_t& cpu, const unsigned vd, const unsigned vs1,
		const unsigned vs2, const bool vm, F&& fold)
	{
		auto& rvv = cpu.registers().rvv();
		const uint64_t vl = rvv.vl();
		if (vl == 0)
			return;
		T acc = element_at<T>(rvv, vs1, 0);
		for (uint64_t i = 0; i < vl; i++) {
			if (element_active(rvv, vm, i)) {
				acc = fold(acc, element_at<T>(rvv, vs2, i));
			}
		}
		element_at<T>(rvv, vd, 0) = acc;
	}

	// Saturate an already-rounded value to the range of E.
	template <typename E>
	static E f2i_saturate(const long double v)
	{
		if constexpr (std::is_signed_v<E>) {
			constexpr long double lo = -(long double)(UINT64_C(1) << (8 * sizeof(E) - 1));
			constexpr long double hi =  (long double)((UINT64_C(1) << (8 * sizeof(E) - 1)) - 1);
			if (v < lo) return (E)(intmax_t)lo;
			if (v > hi) return (E)(intmax_t)hi;
			return (E)(intmax_t)v;
		} else {
			constexpr unsigned long long max_e =
				sizeof(E) == 8 ? ~0ULL
				: (~0ULL << (64 - 8 * sizeof(E))) >> (64 - 8 * sizeof(E));
			constexpr long double hi = (long double)max_e;
			if (v <= 0.0L) return E(0);
			if (v > hi)    return (E)max_e;
			return (E)(unsigned long long)v;
		}
	}

	// Saturating float to integer conversion (NaN converts to 0).
	template <typename E, typename F>
	static E f2i_sat(const F value, const bool round_to_zero)
	{
		if (std::isnan(value)) return E(0);
		const long double v = round_to_zero
			? std::trunc((long double)value) : std::rint((long double)value);
		return f2i_saturate<E>(v);
	}

	// Like f2i_sat, but rounds per the given frm (1=RTZ, 2=RDN, 3=RUP,
	// everything else RNE/RMM).
	template <typename E, typename F>
	static E f2i_frm_sat(const F value, const unsigned frm)
	{
		if (std::isnan(value)) return E(0);
		const long double v =
			frm == 1 ? std::trunc((long double)value) :
			frm == 2 ? std::floor((long double)value) :
			frm == 3 ? std::ceil((long double)value) :
			std::rint((long double)value);
		return f2i_saturate<E>(v);
	}

	// IEEE-754 classification mask for vfclass (bit 9 = qNaN down to
	// bit 0 = -infinity).
	template <typename F>
	static unsigned fp_class_mask(const F value)
	{
		if (std::isnan(value)) {
			constexpr unsigned qshift = sizeof(F) == 4 ? 22 : 51;
			using B = std::conditional_t<sizeof(F) == 4, uint32_t, uint64_t>;
			B bits;
			__builtin_memcpy(&bits, &value, sizeof(bits));
			return (bits >> qshift & 1) ? (1u << 9) : (1u << 8);
		}
		const bool neg = std::signbit(value);
		if (std::isinf(value)) return neg ? 1u : (1u << 7);
		if (value == F(0))     return neg ? (1u << 3) : (1u << 4);
		if (std::isnormal(value)) return neg ? (1u << 1) : (1u << 6);
		return neg ? (1u << 2) : (1u << 5); // subnormal
	}

	// vfwcvt (sel 8-15) / vfncvt (sel 16-23) conversions: the destination
	// element is SEW-sized, the source SEW/2 (widening) or 2*SEW
	// (narrowing). FP16 forms are not implemented.
	template <typename CPU_t>
	static void fp_widen_narrow(CPU_t& cpu, const unsigned vd,
		const unsigned vs2, const bool vm, const unsigned sel)
	{
		auto& rvv = cpu.registers().rvv();
		const unsigned frm = cpu.registers().fcsr().frm;
		switch (rvv.sew()) {
		case 16: // narrowing: f32 -> i16/u16
			switch (sel) {
			case 16: vector_element_loop<uint16_t>(cpu, vd, vm, [&] (uint64_t i) {
					return f2i_frm_sat<uint16_t>(element_at<float>(rvv, vs2, i), frm); });
				return;
			case 17: vector_element_loop<int16_t>(cpu, vd, vm, [&] (uint64_t i) {
					return f2i_frm_sat<int16_t>(element_at<float>(rvv, vs2, i), frm); });
				return;
			case 22: vector_element_loop<uint16_t>(cpu, vd, vm, [&] (uint64_t i) {
					return f2i_sat<uint16_t>(element_at<float>(rvv, vs2, i), true); });
				return;
			case 23: vector_element_loop<int16_t>(cpu, vd, vm, [&] (uint64_t i) {
					return f2i_sat<int16_t>(element_at<float>(rvv, vs2, i), true); });
				return;
			default: // f16 destination: unsupported
				break;
			}
			break;
		case 32:
			if (sel >= 16) { // narrowing: f64/i64/u64 -> SEW
				switch (sel) {
				case 16: vector_element_loop<uint32_t>(cpu, vd, vm, [&] (uint64_t i) {
						return f2i_frm_sat<uint32_t>(element_at<double>(rvv, vs2, i), frm); });
					return;
				case 17: vector_element_loop<int32_t>(cpu, vd, vm, [&] (uint64_t i) {
						return f2i_frm_sat<int32_t>(element_at<double>(rvv, vs2, i), frm); });
					return;
				case 18: vector_element_loop<float>(cpu, vd, vm, [&] (uint64_t i) {
						return (float)element_at<uint64_t>(rvv, vs2, i); });
					return;
				case 19: vector_element_loop<float>(cpu, vd, vm, [&] (uint64_t i) {
						return (float)element_at<int64_t>(rvv, vs2, i); });
					return;
				case 20: vector_element_loop<float>(cpu, vd, vm, [&] (uint64_t i) {
						return (float)element_at<double>(rvv, vs2, i); });
					return;
				case 21: { // vfncvt.rod.f.f.w: round to odd
					using bits_t = uint32_t;
					vector_element_loop<float>(cpu, vd, vm, [&] (uint64_t i) {
						const double v = element_at<double>(rvv, vs2, i);
						float r = (float)v;
						if ((double)r != v) {
							bits_t b;
							__builtin_memcpy(&b, &r, sizeof(b));
							b |= 1;
							__builtin_memcpy(&r, &b, sizeof(r));
						}
						return r;
					});
					return; }
				case 22: vector_element_loop<uint32_t>(cpu, vd, vm, [&] (uint64_t i) {
						return f2i_sat<uint32_t>(element_at<double>(rvv, vs2, i), true); });
					return;
				case 23: vector_element_loop<int32_t>(cpu, vd, vm, [&] (uint64_t i) {
						return f2i_sat<int32_t>(element_at<double>(rvv, vs2, i), true); });
					return;
				}
				break;
			}
			// widening: i16/u16 -> f32 (float sources would be f16)
			switch (sel) {
			case 10: vector_element_loop<float>(cpu, vd, vm, [&] (uint64_t i) {
					return (float)element_at<uint16_t>(rvv, vs2, i); });
				return;
			case 11: vector_element_loop<float>(cpu, vd, vm, [&] (uint64_t i) {
					return (float)element_at<int16_t>(rvv, vs2, i); });
				return;
			}
			break;
		case 64: // widening: f32/u32/i32 -> SEW
			switch (sel) {
			case 8: vector_element_loop<uint64_t>(cpu, vd, vm, [&] (uint64_t i) {
					return f2i_frm_sat<uint64_t>(element_at<float>(rvv, vs2, i), frm); });
				return;
			case 9: vector_element_loop<int64_t>(cpu, vd, vm, [&] (uint64_t i) {
					return f2i_frm_sat<int64_t>(element_at<float>(rvv, vs2, i), frm); });
				return;
			case 10: vector_element_loop<double>(cpu, vd, vm, [&] (uint64_t i) {
					return (double)element_at<uint32_t>(rvv, vs2, i); });
				return;
			case 11: vector_element_loop<double>(cpu, vd, vm, [&] (uint64_t i) {
					return (double)element_at<int32_t>(rvv, vs2, i); });
				return;
			case 12: vector_element_loop<double>(cpu, vd, vm, [&] (uint64_t i) {
					return (double)element_at<float>(rvv, vs2, i); });
				return;
			case 14: vector_element_loop<uint64_t>(cpu, vd, vm, [&] (uint64_t i) {
					return f2i_sat<uint64_t>(element_at<float>(rvv, vs2, i), true); });
				return;
			case 15: vector_element_loop<int64_t>(cpu, vd, vm, [&] (uint64_t i) {
					return f2i_sat<int64_t>(element_at<float>(rvv, vs2, i), true); });
				return;
			}
			break;
		}
		cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION);
	}

	// vzext/vsext.vfN: vd[i] = ext(vs2[i]), source element SEW/N.
	// sel is the vs1 field: 2/4/6 zero-extend from SEW/8/4/2,
	// 3/5/7 sign-extend.
	template <typename D, typename S, typename CPU_t, typename RVV_t>
	static void extension_loop(CPU_t& cpu, RVV_t& rvv,
		const unsigned vd, const unsigned vs2, const bool vm)
	{
		vector_element_loop<D>(cpu, vd, vm, [&] (uint64_t i) {
			return D(element_at<S>(rvv, vs2, i));
		});
	}
	template <typename CPU_t, typename RVV_t>
	static void int_extension(CPU_t& cpu, RVV_t& rvv, const unsigned vd,
		const unsigned vs2, const bool vm, const unsigned sel)
	{
		const unsigned bits = cpu.registers().rvv().sew();
		const unsigned factor = (sel <= 3) ? 8 : (sel <= 5) ? 4 : 2;
		const bool sx = sel & 1;
		switch (bits) {
		case 16:
			if (factor == 2) {
				if (sx) extension_loop<int16_t, int8_t>(cpu, rvv, vd, vs2, vm);
				else    extension_loop<int16_t, uint8_t>(cpu, rvv, vd, vs2, vm);
				return;
			}
			break;
		case 32:
			if (factor == 4) {
				if (sx) extension_loop<int32_t, int8_t>(cpu, rvv, vd, vs2, vm);
				else    extension_loop<int32_t, uint8_t>(cpu, rvv, vd, vs2, vm);
			} else if (factor == 2) {
				if (sx) extension_loop<int32_t, int16_t>(cpu, rvv, vd, vs2, vm);
				else    extension_loop<int32_t, uint16_t>(cpu, rvv, vd, vs2, vm);
			} else break;
			return;
		case 64:
			if (factor == 8) {
				if (sx) extension_loop<int64_t, int8_t>(cpu, rvv, vd, vs2, vm);
				else    extension_loop<int64_t, uint8_t>(cpu, rvv, vd, vs2, vm);
			} else if (factor == 4) {
				if (sx) extension_loop<int64_t, int16_t>(cpu, rvv, vd, vs2, vm);
				else    extension_loop<int64_t, uint16_t>(cpu, rvv, vd, vs2, vm);
			} else if (factor == 2) {
				if (sx) extension_loop<int64_t, int32_t>(cpu, rvv, vd, vs2, vm);
				else    extension_loop<int64_t, uint32_t>(cpu, rvv, vd, vs2, vm);
			} else break;
			return;
		}
		cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION);
	}
} // anonymous namespace

	VECTOR_INSTR(VSETVLI,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32v_instruction vi { instr };
		auto& rvv = cpu.registers().rvv();
		const bool rd_is_x0  = vi.VLI.rd == 0;
		const bool rs1_is_x0 = vi.VLI.rs1 == 0;

		const uint64_t avl =
			!rs1_is_x0 ? (uint64_t)cpu.reg(vi.VLI.rs1) :
			!rd_is_x0  ? ~uint64_t(0) :
			(uint64_t)rvv.vl();

		if (!rvv.set_vtype(vi.VLI.zimm)) {
			// Unsupported vtype: vl=0, vtype.vill=1.
			rvv.set_vl(0);
			if (!rd_is_x0) cpu.reg(vi.VLI.rd) = 0;
			return;
		}
		const uint64_t vlmax = rvv.vlmax();
		const uint64_t vl = avl < vlmax ? avl : vlmax;
		rvv.set_vl(vl);
		if (!rd_is_x0) cpu.reg(vi.VLI.rd) = rvv.vl();
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		const rv32v_instruction vi { instr };
		const uint32_t lmul = vi.VLI.zimm & 0x7;
		static const char lmul_str[8][8] = {
			"1", "2", "4", "8", "?", "1/8", "1/4", "1/2"};
		return snprintf(buffer, len, "VSETVLI %s, %s, e%u, m%s, t%s, m%s",
						RISCV::regname(vi.VLI.rd),
						RISCV::regname(vi.VLI.rs1),
						8u << ((vi.VLI.zimm >> 3) & 0x7),
						lmul_str[lmul],
						(vi.VLI.zimm & 0x40) ? "a" : "u",
						(vi.VLI.zimm & 0x80) ? "a" : "u");
	});

	VECTOR_INSTR(VSETIVLI,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32v_instruction vi { instr };
		auto& rvv = cpu.registers().rvv();
		const bool rd_is_x0 = vi.IVLI.rd == 0;
		const uint64_t avl   = vi.IVLI.uimm;         // 5-bit unsigned AVL
		const uint32_t vtype = vi.IVLI.zimm & 0x3F;  // vlmul+vsew only

		if (!rvv.set_vtype(vtype)) {
			rvv.set_vl(0);
			if (!rd_is_x0) cpu.reg(vi.IVLI.rd) = 0;
			return;
		}
		const uint64_t vlmax = rvv.vlmax();
		const uint64_t vl = avl < vlmax ? avl : vlmax;
		rvv.set_vl(vl);
		if (!rd_is_x0) cpu.reg(vi.IVLI.rd) = rvv.vl();
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		const rv32v_instruction vi { instr };
		const uint32_t lmul = vi.IVLI.zimm & 0x7;
		static const char lmul_str[8][8] = {
			"1", "2", "4", "8", "?", "1/8", "1/4", "1/2"};
		return snprintf(buffer, len, "VSETIVLI %s, 0x%X, e%u, m%s",
						RISCV::regname(vi.IVLI.rd),
						vi.IVLI.uimm,
						8u << ((vi.IVLI.zimm >> 3) & 0x7),
						lmul_str[lmul]);
	});

	VECTOR_INSTR(VSETVL,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32v_instruction vi { instr };
		auto& rvv = cpu.registers().rvv();
		const bool rd_is_x0  = vi.VSETVL.rd == 0;
		const bool rs1_is_x0 = vi.VSETVL.rs1 == 0;

		const uint64_t avl =
			!rs1_is_x0 ? (uint64_t)cpu.reg(vi.VSETVL.rs1) :
			!rd_is_x0  ? ~uint64_t(0) :
			(uint64_t)rvv.vl();

		const auto vtype_reg = cpu.reg(vi.VSETVL.rs2);
		const bool supported =
			(vtype_reg & ~decltype(vtype_reg)(0xFFF)) == 0
			&& rvv.set_vtype((uint32_t)vtype_reg);

		if (!supported) {
			// Bits above vma/vta are reserved: set vill.
			rvv.set_vtype(0xFFF);
			rvv.set_vl(0);
			if (!rd_is_x0) cpu.reg(vi.VSETVL.rd) = 0;
			return;
		}
		const uint64_t vlmax = rvv.vlmax();
		const uint64_t vl = avl < vlmax ? avl : vlmax;
		rvv.set_vl(vl);
		if (!rd_is_x0) cpu.reg(vi.VSETVL.rd) = rvv.vl();
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		const rv32v_instruction vi { instr };
		return snprintf(buffer, len, "VSETVL %s, %s, %s",
						RISCV::regname(vi.VSETVL.rd),
						RISCV::regname(vi.VSETVL.rs1),
						RISCV::regname(vi.VSETVL.rs2));
	});

	namespace
	{
		// Validation for unit-stride loads/stores; returns the EMUL
		// shift for the instruction's EEW, or traps.
		template <typename CPU_t>
		static int check_unit_stride(CPU_t& cpu, const rv32v_instruction& vi,
			const unsigned eew_bytes, const unsigned vreg)
		{
			require_valid_vtype(cpu);
			auto& rvv = cpu.registers().rvv();
			// Unit-stride only: mop=00, umop=0 (01000 is the
			// whole-register form), no segments, no MEW.
			if (vi.VL.mew || vi.VL.nf != 0 || vi.VL.mop != 0 || vi.VL.lumop != 0)
				cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION);

			const int emul = rvv.lmul_shift()
				+ (int)(bits_of(eew_bytes) - bits_of(rvv.sew() / 8));
			if (emul < 0 || emul > 3)
				cpu.trigger_exception(ILLEGAL_OPCODE);

			const unsigned regs = 1u << emul;
			if (regs > 1 && ((vreg % regs) != 0 || vreg + regs > 32))
				cpu.trigger_exception(ILLEGAL_OPCODE);

			const uint64_t group_vlmax =
				(uint64_t(VectorLane::size()) / eew_bytes) << emul;
			if (rvv.vl() > group_vlmax)
				cpu.trigger_exception(ILLEGAL_OPCODE);
			return emul;
		}

		// Unit-stride load for one EEW, honoring vl and mask. memcpy-based:
		// vector memory accesses have no alignment requirement.
		template <typename T, typename CPU_t>
		static void unit_stride_load(CPU_t& cpu, const unsigned vd, const bool vm,
			const typename CPU_t::address_t base, const int emul)
		{
			auto& rvv = cpu.registers().rvv();
			auto& memory = cpu.machine().memory;
			const uint64_t vl = rvv.vl();
			const unsigned regs = 1u << emul;
			constexpr unsigned per_reg = VectorLane::size() / sizeof(T);

			if (vm && vl == per_reg * (uint64_t)regs) {
				for (unsigned r = 0; r < regs; r++)
					memory.memcpy_out(&rvv.get(vd + r),
						base + r * VectorLane::size(), VectorLane::size());
				return;
			}
			for (uint64_t i = 0; i < vl; i++) {
				if (element_active(rvv, vm, i)) {
					T value;
					memory.memcpy_out(&value, base + i * sizeof(T), sizeof(T));
					element_at<T>(rvv, vd, i) = value;
				}
			}
		}

		// Unit-stride store for one EEW, honoring vl and mask.
		template <typename T, typename CPU_t>
		static void unit_stride_store(CPU_t& cpu, const unsigned vs3, const bool vm,
			const typename CPU_t::address_t base, const int emul)
		{
			auto& rvv = cpu.registers().rvv();
			auto& memory = cpu.machine().memory;
			const uint64_t vl = rvv.vl();
			const unsigned regs = 1u << emul;
			constexpr unsigned per_reg = VectorLane::size() / sizeof(T);

			if (vm && vl == per_reg * (uint64_t)regs) {
				for (unsigned r = 0; r < regs; r++)
					memory.memcpy(base + r * VectorLane::size(),
						&rvv.get(vs3 + r), VectorLane::size());
				return;
			}
			for (uint64_t i = 0; i < vl; i++) {
				if (element_active(rvv, vm, i)) {
					const T value = element_at<T>(rvv, vs3, i);
					memory.memcpy(base + i * sizeof(T), &value, sizeof(T));
				}
			}
		}
	}

	VECTOR_INSTR(VLE32,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32v_instruction vi { instr };
		const unsigned eew_bytes = 1u << bits_lookup(vi.VL.width);
		const int emul = check_unit_stride(cpu, vi, eew_bytes, vi.VL.vd);
		const auto base = cpu.reg(vi.VL.rs1);

		switch (eew_bytes) {
			case 1: unit_stride_load<uint8_t> (cpu, vi.VL.vd, vi.VL.vm, base, emul); break;
			case 2: unit_stride_load<uint16_t>(cpu, vi.VL.vd, vi.VL.vm, base, emul); break;
			case 4: unit_stride_load<uint32_t>(cpu, vi.VL.vd, vi.VL.vm, base, emul); break;
			case 8: unit_stride_load<uint64_t>(cpu, vi.VL.vd, vi.VL.vm, base, emul); break;
			default: cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		const rv32v_instruction vi { instr };
		return snprintf(buffer, len, "VLE%d.V v%u, (%s)",
						8 << bits_lookup(vi.VL.width),
						vi.VL.vd, RISCV::regname(vi.VL.rs1));
	});

	VECTOR_INSTR(VSE32,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32v_instruction vi { instr };
		const unsigned eew_bytes = 1u << bits_lookup(vi.VS.width);
		const int emul = check_unit_stride(cpu, vi, eew_bytes, vi.VS.vs3);
		const auto base = cpu.reg(vi.VS.rs1);

		switch (eew_bytes) {
			case 1: unit_stride_store<uint8_t> (cpu, vi.VS.vs3, vi.VS.vm, base, emul); break;
			case 2: unit_stride_store<uint16_t>(cpu, vi.VS.vs3, vi.VS.vm, base, emul); break;
			case 4: unit_stride_store<uint32_t>(cpu, vi.VS.vs3, vi.VS.vm, base, emul); break;
			case 8: unit_stride_store<uint64_t>(cpu, vi.VS.vs3, vi.VS.vm, base, emul); break;
			default: cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION);
		}
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		const rv32v_instruction vi { instr };
		return snprintf(buffer, len, "VSE%d.V v%u, (%s)",
						8 << bits_lookup(vi.VS.width),
						vi.VS.vs3, RISCV::regname(vi.VS.rs1));
	});

 	static const char *OPINAMES[64] = {
		"VADD", "???", "VSUB", "VRSUB", "VMINU", "VMIN", "VMAXU", "VMAX",
		"???", "VAND", "VOR", "VXOR", "VRGATHER", "???", "VSLIDE1UP", "VSLIDE1DOWN",
		"VADC", "VMADC", "VSBC", "VMSBC", "???", "???", "???", "VMERGE",
		"VMSEQ", "VMSNE", "VMSLTU", "VMSLT", "VMSLEU", "VMSLE", "VMSGTU", "VMSGT",
		"VSADDU", "VSADD", "VSSUBU", "VSSUB", "???", "VSLL", "???", "VMVNR",
		"VSRL", "VSRA", "???", "???", "???", "???", "???", "???",
		"???", "???", "???", "???", "???", "???", "???", "???",
		"???", "???", "???", "???", "???", "???", "???", "???"};
	static const char *OPMNAMES[64] = {
		"VREDSUM", "VREDAND", "VREDOR", "VREDXOR", "VREDMINU", "VREDMIN", "VREDMAXU", "VREDMAX",
		"???", "???", "???", "???", "???", "???", "???", "???",
		"VMV.X.S", "???", "???", "???", "VMSBF", "???", "VMSOF", "VMSIF",
		"VMANDN", "VMAND", "VMOR", "VMXOR", "???", "VMNAND", "VMNOR", "VMXNOR",
		"???", "???", "???", "???", "???", "VMUL", "VMULH", "VMULHU",
		"???", "VMULHSU", "???", "VMACC", "VNMSAC", "VMADD", "VNMSUB", "???",
		"???", "???", "???", "???", "???", "???", "???", "???",
		"???", "???", "???", "???", "???", "???", "???", "???"};
	static const char *OPFNAMES[64] = {
		"VFADD", "VFREDUSUM", "VFSUB", "VFREDOSUM", "VFMIN", "VFREDMIN", "VFMAX", "VFREDMAX",
		"VFSGNJ", "VFSGNJN", "VFSGNJX", "???", "???", "???", "VFSLIDE1UP", "VFSLIDE1DOWN",
		"VFMV.F.S", "???", "VFCVT", "VFSQRT", "???", "???", "???", "???",
		"VMFEQ", "VMFLE", "VMFLT", "???", "VMFNE", "VMFGT", "VMFGE", "???",
		"VFDIV", "VFRDIV", "???", "???", "VFMUL", "???", "???", "VFRSUB",
		"VFMADD", "VFNMADD", "VFMSUB", "VFNMSUB", "VFMACC", "VFNMACC", "VFMSAC", "VFNMSAC",
		"???", "???", "???", "???", "???", "???", "???", "???",
		"???", "???", "???", "???", "???", "???", "???", "???"};

	/* OPIVV: vd = vs2 OP vs1 */
	VECTOR_INSTR(VOPI_VV,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32v_instruction vi { instr };
		require_valid_vtype(cpu);
		auto& rvv = cpu.registers().rvv();
		const unsigned vd = vi.OPVV.vd, vs1 = vi.OPVV.vs1, vs2 = vi.OPVV.vs2;
		const bool vm = vi.OPVV.vm;
		check_register_group(cpu, vd);
		check_register_group(cpu, vs1);
		check_register_group(cpu, vs2);

		switch (vi.OPVV.funct6) {
		case 0b000000: // VADD.VV
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t i) {
					return element_at<E>(rvv, vs2, i) + element_at<E>(rvv, vs1, i);
				});
			});
			return;
		case 0b000010: // VSUB.VV
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t i) {
					return element_at<E>(rvv, vs2, i) - element_at<E>(rvv, vs1, i);
				});
			});
			return;
		case 0b000100: // VMINU.VV
		case 0b000101: // VMIN.VV
		case 0b000110: // VMAXU.VV
		case 0b000111: // VMAX.VV
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				using U = std::make_unsigned_t<E>;
				const unsigned funct6 = vi.OPVV.funct6;
				vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t i) {
					const U a = element_at<E>(rvv, vs2, i), b = element_at<E>(rvv, vs1, i);
					switch (funct6) {
						case 0b000100: return (E)(a < b ? a : b);
						case 0b000101: return (E)((E)a < (E)b ? a : b);
						case 0b000110: return (E)(a > b ? a : b);
						default:        return (E)((E)a > (E)b ? a : b);
					}
				});
			});
			return;
		case 0b001001: // VAND.VV
		case 0b001010: // VOR.VV
		case 0b001011: // VXOR.VV
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				const unsigned funct6 = vi.OPVV.funct6;
				vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t i) {
					const E a = element_at<E>(rvv, vs2, i), b = element_at<E>(rvv, vs1, i);
					if (funct6 == 0b001001) return E(a & b);
					if (funct6 == 0b001010) return E(a | b);
					return E(a ^ b);
				});
			});
			return;
		case 0b001100: // VRGATHER.VV: vd[i] = (vs1[i] >= VLMAX) ? 0 : vs2[vs1[i]]
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				using U = std::make_unsigned_t<E>;
				const uint64_t vlmax = rvv.vlmax();
				vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t i) {
					const uint64_t idx = (U)element_at<E>(rvv, vs1, i);
					return idx < vlmax ? element_at<E>(rvv, vs2, idx) : E(0);
				});
			});
			return;
		case 0b010000: // VADC.VVM: vd = vs2 + vs1 + v0 (masked encoding)
			if (!vm) {
				int_sew_dispatch(cpu, [&] (auto tag) {
					using E = decltype(tag);
					using U = std::make_unsigned_t<E>;
					vector_element_loop<E>(cpu, vd, true, [&] (uint64_t i) {
						return (E)((U)element_at<E>(rvv, vs2, i)
								+ (U)element_at<E>(rvv, vs1, i)
								+ rvv.get(0).mask(i));
					});
				});
			}
			break; // vm=1 is reserved
		case 0b010001: // VMADC.VVM (carry-in) / VMADC.VV
			// No masked form: all elements are written; vm selects
			// whether v0 is the carry input.
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				using U = std::make_unsigned_t<E>;
				mask_dest_loop(cpu, vd, true, [&] (uint64_t i) {
					const U a = element_at<E>(rvv, vs2, i), b = element_at<E>(rvv, vs1, i);
					const U cin = vm ? U(0) : (rvv.get(0).mask(i) ? 1u : 0u);
					const U sum = a + b + cin;
					return sum < a || (cin != 0 && sum == a);
				});
			});
			return;
		case 0b010010: // VSBC.VVM: vd = vs2 - vs1 - v0 (masked encoding)
			if (!vm) {
				int_sew_dispatch(cpu, [&] (auto tag) {
					using E = decltype(tag);
					using U = std::make_unsigned_t<E>;
					vector_element_loop<E>(cpu, vd, true, [&] (uint64_t i) {
						return (E)((U)element_at<E>(rvv, vs2, i)
								- (U)element_at<E>(rvv, vs1, i)
								- rvv.get(0).mask(i));
					});
				});
			}
			break; // vm=1 is reserved
		case 0b010011: // VMSBC.VVM (borrow-in) / VMSBC.VV
			// VMSBC has no masked form (see VMADC above).
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				using U = std::make_unsigned_t<E>;
				mask_dest_loop(cpu, vd, true, [&] (uint64_t i) {
					const U a = element_at<E>(rvv, vs2, i), b = element_at<E>(rvv, vs1, i);
					const U bin = vm ? U(0) : (rvv.get(0).mask(i) ? 1u : 0u);
					return a < b || (a == b && bin != 0);
				});
			});
			return;
		case 0b010111: // VMERGE.VVM (vm=0) / VMV.V.V (vm=1)
			// No masked form: vm=0 selects on v0, vm=1 is VMV.
			if (!vm) {
				int_sew_dispatch(cpu, [&] (auto tag) {
					using E = decltype(tag);
					vector_element_loop<E>(cpu, vd, true, [&] (uint64_t i) {
						return rvv.get(0).mask(i)
							? element_at<E>(rvv, vs1, i)
							: element_at<E>(rvv, vs2, i);
					});
				});
			} else {
				int_sew_dispatch(cpu, [&] (auto tag) {
					using E = decltype(tag);
					vector_element_loop<E>(cpu, vd, true, [&] (uint64_t i) {
						return element_at<E>(rvv, vs1, i);
					});
				});
			}
			return;
		case 0b011000: // VMSEQ.VV
		case 0b011001: // VMSNE.VV
		case 0b011010: // VMSLTU.VV
		case 0b011011: // VMSLT.VV
		case 0b011100: // VMSLEU.VV
		case 0b011101: // VMSLE.VV
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				using U = std::make_unsigned_t<E>;
				const unsigned funct6 = vi.OPVV.funct6;
				mask_dest_loop(cpu, vd, vm, [&] (uint64_t i) {
					const U a = element_at<E>(rvv, vs2, i), b = element_at<E>(rvv, vs1, i);
					switch (funct6) {
						case 0b011000: return a == b;
						case 0b011001: return a != b;
						case 0b011010: return a < b;
						case 0b011011: return (E)a < (E)b;
						case 0b011100: return a <= b;
						default:        return (E)a <= (E)b;
					}
				});
			});
			return;
		case 0b100000: // VSADDU.VV
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				using U = std::make_unsigned_t<E>;
				vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t i) {
					const U a = element_at<E>(rvv, vs2, i), b = element_at<E>(rvv, vs1, i);
					const U sum = a + b;
					return (E)(sum < a ? ~U(0) : sum); // saturate
				});
			});
			return;
		case 0b100001: // VSADD.VV
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				using U = std::make_unsigned_t<E>;
				constexpr U sign_bit = U(1) << (8 * sizeof(E) - 1);
				vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t i) {
					const E a = element_at<E>(rvv, vs2, i), b = element_at<E>(rvv, vs1, i);
					const U sum = (U)a + (U)b;
					if ((a < 0) == (b < 0) && ((sum & sign_bit) != 0) != (a < 0))
						return (E)(a < 0 ? sign_bit : ~sign_bit); // saturate
					return (E)sum;
				});
			});
			return;
		case 0b100010: // VSSUBU.VV
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				using U = std::make_unsigned_t<E>;
				vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t i) {
					const U a = element_at<E>(rvv, vs2, i), b = element_at<E>(rvv, vs1, i);
					return (E)(a < b ? U(0) : a - b); // saturate
				});
			});
			return;
		case 0b100011: // VSSUB.VV
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				using U = std::make_unsigned_t<E>;
				constexpr U sign_bit = U(1) << (8 * sizeof(E) - 1);
				vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t i) {
					const E a = element_at<E>(rvv, vs2, i), b = element_at<E>(rvv, vs1, i);
					const U sub = (U)a - (U)b;
					if ((a < 0) != (b < 0) && ((sub & sign_bit) != 0) != (a < 0))
						return (E)(a < 0 ? sign_bit : ~sign_bit); // saturate
					return (E)sub;
				});
			});
			return;
		case 0b100101: // VSLL.VV
		case 0b101000: // VSRL.VV
		case 0b101001: // VSRA.VV
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				using U = std::make_unsigned_t<E>;
				constexpr U shamt_mask = 8 * sizeof(E) - 1;
				const unsigned funct6 = vi.OPVV.funct6;
				vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t i) {
					const U sh = element_at<E>(rvv, vs1, i) & shamt_mask;
					const U a = element_at<E>(rvv, vs2, i);
					if (funct6 == 0b100101) return (E)(a << sh);
					if (funct6 == 0b101000) return (E)(a >> sh);
					return (E)((E)a >> sh);
				});
			});
			return;
		} // switch
		cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		const rv32v_instruction vi { instr };
		return snprintf(buffer, len, "%s.VV v%u, v%u, v%u",
						OPINAMES[vi.OPVV.funct6],
						vi.OPVV.vd, vi.OPVV.vs2, vi.OPVV.vs1);
	});

	/* OPIVI / OPIVX: vd = vs2 OP imm / x[rs1].
	 * OPIVX (funct3=100) shares this slot; the scalar then comes from
	 * an integer register instead of a 5-bit immediate. */
	VECTOR_INSTR(VOPI_VI,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32v_instruction vi { instr };
		require_valid_vtype(cpu);
		auto& rvv = cpu.registers().rvv();
		const unsigned vd = vi.OPVI.vd, vs2 = vi.OPVI.vs2;
		const bool vm = vi.OPVI.vm;
		const bool is_vx = instr.Itype.funct3 == 0b100;
		check_register_group(cpu, vd);
		check_register_group(cpu, vs2);

		// Scalar immediates: sign-extended (arith/cmp) and zero-extended
		// (shifts, unsigned compares). For OPIVX the x register value
		// is truncated to SEW bits (treated as signed).
		const uint32_t imm5 = vi.OPVI.imm;
		const uint64_t simm = is_vx ? (uint64_t)cpu.reg(imm5)
			: (uint64_t)((int64_t)(imm5 ^ 0x10) - 0x10);
		const uint64_t zimm = is_vx ? (uint64_t)cpu.reg(imm5) : imm5;

		switch (vi.OPVI.funct6) {
		case 0b000000: // VADD.VI / VADD.VX
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t i) {
					return element_at<E>(rvv, vs2, i) + (E)simm;
				});
			});
			return;
		case 0b000011: // VRSUB.VI / VRSUB.VX: vd = imm - vs2
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t i) {
					return (E)simm - element_at<E>(rvv, vs2, i);
				});
			});
			return;
		case 0b001001: // VAND
		case 0b001010: // VOR
		case 0b001011: // VXOR
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				const unsigned funct6 = vi.OPVI.funct6;
				vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t i) {
					const E a = element_at<E>(rvv, vs2, i), b = (E)simm;
					if (funct6 == 0b001001) return E(a & b);
					if (funct6 == 0b001010) return E(a | b);
					return E(a ^ b);
				});
			});
			return;
		case 0b001100: // VRGATHER.VI / VRGATHER.VX
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				const uint64_t vlmax = rvv.vlmax();
				vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t) {
					return zimm < vlmax ? element_at<E>(rvv, vs2, zimm) : E(0);
				});
			});
			return;
		case 0b001110: { // VSLIDEUP.VI / VSLIDEUP.VX
			const uint64_t offset = zimm;
			const uint64_t vl = rvv.vl();
			if (offset < vl) {
				int_sew_dispatch(cpu, [&] (auto tag) {
					using E = decltype(tag);
					for (uint64_t i = offset; i < vl; i++) {
						if (element_active(rvv, vm, i))
							element_at<E>(rvv, vd, i) = element_at<E>(rvv, vs2, i - offset);
					}
				});
			}
			return; }
		case 0b001111: // VSLIDEDOWN.VI / VSLIDEDOWN.VX
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				const uint64_t vl = rvv.vl();
				const uint64_t vlmax = rvv.vlmax();
				for (uint64_t i = 0; i < vl; i++) {
					if (element_active(rvv, vm, i)) {
						const uint64_t src = i + zimm;
						element_at<E>(rvv, vd, i) =
							src < vlmax ? element_at<E>(rvv, vs2, src) : E(0);
					}
				}
			});
			return;
		case 0b010111: // VMERGE.VIM/VXM (vm=0) / VMV.V.I/X (vm=1)
			// VMERGE has no masked form (see VOPI_VV).
			if (!vm) {
				int_sew_dispatch(cpu, [&] (auto tag) {
					using E = decltype(tag);
					vector_element_loop<E>(cpu, vd, true, [&] (uint64_t i) {
						return rvv.get(0).mask(i)
							? (E)simm : element_at<E>(rvv, vs2, i);
					});
				});
			} else {
				int_sew_dispatch(cpu, [&] (auto tag) {
					using E = decltype(tag);
					vector_element_loop<E>(cpu, vd, true, [&] (uint64_t) {
						return (E)simm;
					});
				});
			}
			return;
		case 0b011000: // VMSEQ
		case 0b011001: // VMSNE
		case 0b011010: // VMSLTU (only .vx for scalar form)
		case 0b011011: // VMSLT
		case 0b011100: // VMSLEU (unsigned imm) / VMSLE
		case 0b011101:
		case 0b011110: // VMSGTU (unsigned imm) / VMSGT
		case 0b011111:
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				using U = std::make_unsigned_t<E>;
				const unsigned funct6 = vi.OPVI.funct6;
				// Unsigned .vi compares (vmsleu, vmsgtu) use zero-extended
				// immediates; the rest are sign-extended.
				const E scalar = (!is_vx && (funct6 == 0b011100 || funct6 == 0b011110))
					? (E)zimm : (E)simm;
				mask_dest_loop(cpu, vd, vm, [&] (uint64_t i) {
					const U a = element_at<E>(rvv, vs2, i);
					switch (funct6) {
						case 0b011000: return a == (U)scalar;
						case 0b011001: return a != (U)scalar;
						case 0b011010: return a <  (U)scalar;
						case 0b011011: return (E)a <  scalar;
						case 0b011100: return a <= (U)scalar;
						case 0b011101: return (E)a <= scalar;
						case 0b011110: return a >  (U)scalar;
						default:        return (E)a >  scalar;
					}
				});
			});
			return;
		case 0b100000: // VSADDU.VI/VX
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				using U = std::make_unsigned_t<E>;
				vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t i) {
					const U a = element_at<E>(rvv, vs2, i), b = (U)(E)simm;
					const U sum = a + b;
					return (E)(sum < a ? ~U(0) : sum); // saturate
				});
			});
			return;
		case 0b100001: // VSADD.VI/VX
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				using U = std::make_unsigned_t<E>;
				constexpr U sign_bit = U(1) << (8 * sizeof(E) - 1);
				vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t i) {
					const E a = element_at<E>(rvv, vs2, i), b = (E)simm;
					const U sum = (U)a + (U)b;
					if ((a < 0) == (b < 0) && ((sum & sign_bit) != 0) != (a < 0))
						return (E)(a < 0 ? sign_bit : ~sign_bit); // saturate
					return (E)sum;
				});
			});
			return;
		case 0b100010: // VSSUBU.VI/VX
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				using U = std::make_unsigned_t<E>;
				vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t i) {
					const U a = element_at<E>(rvv, vs2, i), b = (U)(E)simm;
					return (E)(a < b ? U(0) : a - b); // saturate
				});
			});
			return;
		case 0b100011: // VSSUB.VI/VX
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				using U = std::make_unsigned_t<E>;
				constexpr U sign_bit = U(1) << (8 * sizeof(E) - 1);
				vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t i) {
					const E a = element_at<E>(rvv, vs2, i), b = (E)simm;
					const U sub = (U)a - (U)b;
					if ((a < 0) != (b < 0) && ((sub & sign_bit) != 0) != (a < 0))
						return (E)(a < 0 ? sign_bit : ~sign_bit); // saturate
					return (E)sub;
				});
			});
			return;
		case 0b100101: // VSLL (zimm shift amount)
		case 0b101000: // VSRL
		case 0b101001: // VSRA
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				using U = std::make_unsigned_t<E>;
				constexpr U shamt_mask = 8 * sizeof(E) - 1;
				const unsigned funct6 = vi.OPVI.funct6;
				vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t i) {
					const U sh = zimm & shamt_mask;
					const U a = element_at<E>(rvv, vs2, i);
					if (funct6 == 0b100101) return (E)(a << sh);
					if (funct6 == 0b101000) return (E)(a >> sh);
					return (E)((E)a >> sh);
				});
			});
			return;
		case 0b100111: { // VMV<nr>R.V: whole vector register move
			// Copies nregs whole registers, ignoring vl, vtype and mask.
			if (is_vx || !vm) break;
			const unsigned nregs = imm5 + 1;
			if (nregs != 1 && nregs != 2 && nregs != 4 && nregs != 8)
				break;
			if ((vd % nregs) != 0 || (vs2 % nregs) != 0
				|| vd + nregs > 32 || vs2 + nregs > 32)
				cpu.trigger_exception(ILLEGAL_OPCODE);
			if (vd != vs2) {
				for (unsigned r = 0; r < nregs; r++)
					rvv.get(vd + r) = rvv.get(vs2 + r);
			}
			return; }
		case 0b101100: { // VNCVT.X.X.W (OPIVX with rs1=x0)
			// vd[i] = low SEW bits of the 2*SEW source element
			if (is_vx && imm5 == 0) {
				switch (rvv.sew()) {
				case 16: vector_element_loop<uint16_t>(cpu, vd, vm, [&] (uint64_t i) {
						return (uint16_t)element_at<uint32_t>(rvv, vs2, i); });
					return;
				case 32: vector_element_loop<uint32_t>(cpu, vd, vm, [&] (uint64_t i) {
						return (uint32_t)element_at<uint64_t>(rvv, vs2, i); });
					return;
				}
			}
			break; } // the .vi form and SEW=64 are reserved
		} // switch
		cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION);
		},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		const rv32v_instruction vi { instr };
		if (instr.Itype.funct3 == 0b100) {
			return snprintf(buffer, len, "%s.VX v%u, v%u, %s",
							OPINAMES[vi.OPVI.funct6],
							vi.OPVI.vd, vi.OPVI.vs2,
							RISCV::regname(vi.OPVI.imm));
		}
		return snprintf(buffer, len, "%s.VI v%u, v%u, 0x%X",
						OPINAMES[vi.OPVI.funct6],
						vi.OPVI.vd, vi.OPVI.vs2, vi.OPVI.imm);
	});

	/* OPMVV / OPMVX: reductions, mask operations, scalar moves, slide1 */
	VECTOR_INSTR(VOPM_VV,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32v_instruction vi { instr };
		require_valid_vtype(cpu);
		auto& rvv = cpu.registers().rvv();
		const unsigned vd = vi.OPVV.vd, vs1 = vi.OPVV.vs1, vs2 = vi.OPVV.vs2;
		const bool vm = vi.OPVV.vm;
		const bool is_vx = instr.Itype.funct3 == 0b110;
		check_register_group(cpu, vs2);

		switch (vi.OPVV.funct6) {
		case 0b000000: // VREDSUM.VS
		case 0b000001: // VREDAND.VS
		case 0b000010: // VREDOR.VS
		case 0b000011: // VREDXOR.VS
		case 0b000100: // VREDMINU.VS
		case 0b000101: // VREDMIN.VS
		case 0b000110: // VREDMAXU.VS
		case 0b000111: // VREDMAX.VS
			if (is_vx) break;
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				using U = std::make_unsigned_t<E>;
				const unsigned funct6 = vi.OPVV.funct6;
				reduction_loop<E>(cpu, vd, vs1, vs2, vm, [&] (E acc, E x) -> E {
					switch (funct6) {
						case 0b000000: return acc + x;
						case 0b000001: return acc & x;
						case 0b000010: return acc | x;
						case 0b000011: return acc ^ x;
						case 0b000100: { const U a = acc, b = x; return a < b ? acc : x; }
						case 0b000101: return acc < x ? acc : x;
						case 0b000110: { const U a = acc, b = x; return a > b ? acc : x; }
						default:        return acc > x ? acc : x;
					}
				});
			});
			return;
		case 0b010010: { // VZEXT/VSEXT.vfN (integer extension)
			if (is_vx) break;
			check_register_group(cpu, vd);
			if (vs1 >= 0b00010 && vs1 <= 0b00111) {
				int_extension(cpu, rvv, vd, vs2, vm, vs1);
				return;
			}
			break; }
		case 0b001110: { // VSLIDE1UP.VX
			if (!is_vx) break;
			check_register_group(cpu, vd);
			const uint64_t vl = rvv.vl();
			if (vl > 0) {
				int_sew_dispatch(cpu, [&] (auto tag) {
					using E = decltype(tag);
					for (uint64_t i = vl - 1; i > 0; i--) {
						if (element_active(rvv, vm, i))
							element_at<E>(rvv, vd, i) = element_at<E>(rvv, vs2, i - 1);
					}
					if (element_active(rvv, vm, 0))
						element_at<E>(rvv, vd, 0) = (E)cpu.reg(vs1);
				});
			}
			return; }
		case 0b001111: { // VSLIDE1DOWN.VX
			if (!is_vx) break;
			check_register_group(cpu, vd);
			const uint64_t vl = rvv.vl();
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				for (uint64_t i = 0; i + 1 < vl; i++) {
					if (element_active(rvv, vm, i))
						element_at<E>(rvv, vd, i) = element_at<E>(rvv, vs2, i + 1);
				}
				if (vl > 0 && element_active(rvv, vm, vl - 1))
					element_at<E>(rvv, vd, vl - 1) = (E)cpu.reg(vs1);
			});
			return; }
		case 0b010000:
			if (is_vx) { // VMV.S.X: vd[0] = x[rs1] (vs2 must be 0)
				if (vi.OPVV.vs2 != 0) break;
				check_register_group(cpu, vd);
				if (rvv.vl() > 0) {
					int_sew_dispatch(cpu, [&] (auto tag) {
						using E = decltype(tag);
						element_at<E>(rvv, vd, 0) = (E)cpu.reg(vs1);
					});
				}
				return;
			}
			switch (vs1) {
			case 0b00000: { // VMV.X.S: x[rd] = sext(vs2[0])
				int_sew_dispatch(cpu, [&] (auto tag) {
					using E = decltype(tag);
					cpu.reg(vd) = element_at<E>(rvv, vs2, 0);
				});
				return; }
			case 0b10000: { // VCPOP.M: x[rd] = count of set mask bits
				const uint64_t vl = rvv.vl();
				unsigned count = 0;
				for (uint64_t i = 0; i < vl; i++)
					if (element_active(rvv, vm, i) && rvv.get(vs2).mask(i))
						count++;
				cpu.reg(vd) = count;
				return; }
			case 0b10001: { // VFIRST.M: index of lowest set mask bit
				const uint64_t vl = rvv.vl();
				cpu.reg(vd) = -1;
				for (uint64_t i = 0; i < vl; i++) {
					if (element_active(rvv, vm, i) && rvv.get(vs2).mask(i)) {
						cpu.reg(vd) = i;
						break;
					}
				}
				return; }
			}
			break;
		case 0b010100: { // VMSBF / VMSOF / VMSIF
			if (is_vx) break;
			check_register_group(cpu, vd);
			const uint64_t vl = rvv.vl();
			bool seen_set = false;
			auto& src = rvv.get(vs2);
			auto& dest = rvv.get(vd);
			for (uint64_t i = 0; i < vl; i++) {
				if (element_active(rvv, vm, i)) {
					if (seen_set) {
						dest.set_mask(i, false);
					} else if (src.mask(i)) {
						dest.set_mask(i, vs1 != 0b00001); // SOF/SIF include it
						seen_set = true;
					} else {
						dest.set_mask(i, true);
					}
				}
			}
			return; }
		case 0b011000: // VMANDN:  vd = vs2 & ~vs1
		case 0b011001: // VMAND:   vd = vs2 & vs1
		case 0b011010: // VMOR:    vd = vs2 | vs1
		case 0b011011: // VMXOR:   vd = vs2 ^ vs1
		case 0b011101: // VMNAND:  vd = ~(vs2 & vs1)
		case 0b011110: // VMNOR:   vd = ~(vs2 | vs1)
		case 0b011111: // VMXNOR:  vd = ~(vs2 ^ vs1)
			if (is_vx) break;
			// These operate on all mask bits, regardless of vl.
			for (unsigned b = 0; b < VectorLane::size(); b++) {
				const uint8_t a = rvv.get(vs2).u8[b], c = rvv.get(vs1).u8[b];
				uint8_t r;
				switch (vi.OPVV.funct6) {
					case 0b011000: r = a & ~c;  break;
					case 0b011001: r = a & c;   break;
					case 0b011010: r = a | c;   break;
					case 0b011011: r = a ^ c;   break;
					case 0b011101: r = ~(a & c); break;
					case 0b011110: r = ~(a | c); break;
					default:        r = ~(a ^ c); break;
				}
				rvv.get(vd).u8[b] = r;
			}
			return;
		case 0b100101: // VMUL.VV/VX (lower SEW bits)
		case 0b100110: // VMULH (signed * signed, high half)
		case 0b100111: // VMULHU (unsigned, high half)
		case 0b101001: // VMULHSU (vs2 signed, vs1 unsigned)
		case 0b101011: // VMACC: vd = +(vs1 * vs2) + vd
		case 0b101100: // VNMSAC: vd = -(vs1 * vs2) + vd
		case 0b101101: // VMADD: vd = +(vs1 * vd) + vs2
		case 0b101110: // VNMSUB: vd = -(vs1 * vd) + vs2
			check_register_group(cpu, vd);
			if (!is_vx) check_register_group(cpu, vs1);
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				using U = std::make_unsigned_t<E>;
				constexpr unsigned bits = 8 * sizeof(E);
				const unsigned funct6 = vi.OPVV.funct6;
				vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t i) {
					const U a = element_at<E>(rvv, vs2, i);
					const U b = is_vx ? (U)(E)cpu.reg(vs1)
					                  : (U)element_at<E>(rvv, vs1, i);
					const U c = element_at<E>(rvv, vd, i);
					switch (funct6) {
						case 0b100101: return (E)(U(a) * U(b));
						case 0b100110: return (E)((__int128_t(E(a)) * __int128_t(E(b))) >> bits);
						case 0b100111: return (E)((__uint128_t(U(a)) * __uint128_t(U(b))) >> bits);
						case 0b101001: return (E)((__int128_t(E(a)) * __uint128_t(U(b))) >> bits);
						case 0b101011: return (E)(U(a) * U(b) + c);
						case 0b101100: return (E)(c - U(a) * U(b));
						case 0b101101: return (E)(U(b) * c + a);
						default:        return (E)(a - U(b) * c);
					}
				});
			});
			return;
		} // switch
		cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		const rv32v_instruction vi { instr };
		if (instr.Itype.funct3 == 0b110) {
			return snprintf(buffer, len, "%s.VX v%u, v%u, %s",
							OPMNAMES[vi.OPVV.funct6],
							vi.OPVV.vd, vi.OPVV.vs2,
							RISCV::regname(vi.OPVV.vs1));
		}
		return snprintf(buffer, len, "%s.VS/MM v%u, v%u, v%u",
						OPMNAMES[vi.OPVV.funct6],
						vi.OPVV.vd, vi.OPVV.vs2, vi.OPVV.vs1);
	});

	/* OPFVV: vd = vs2 OP vs1 */
	VECTOR_INSTR(VOPF_VV,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32v_instruction vi { instr };
		require_valid_vtype(cpu);
		auto& rvv = cpu.registers().rvv();
		const unsigned vd = vi.OPVV.vd, vs1 = vi.OPVV.vs1, vs2 = vi.OPVV.vs2;
		const bool vm = vi.OPVV.vm;
		check_register_group(cpu, vd);
		check_register_group(cpu, vs1);
		check_register_group(cpu, vs2);

		switch (vi.OPVV.funct6) {
		case 0b000000: // VFADD.VV
			fp_sew_dispatch(cpu, [&] (auto tag) {
				using F = decltype(tag);
				vector_element_loop<F>(cpu, vd, vm, [&] (uint64_t i) {
					return element_at<F>(rvv, vs2, i) + element_at<F>(rvv, vs1, i);
				});
			});
			return;
		case 0b000010: // VFSUB.VV
			fp_sew_dispatch(cpu, [&] (auto tag) {
				using F = decltype(tag);
				vector_element_loop<F>(cpu, vd, vm, [&] (uint64_t i) {
					return element_at<F>(rvv, vs2, i) - element_at<F>(rvv, vs1, i);
				});
			});
			return;
		case 0b000001: // VFREDUSUM.VS
		case 0b000011: // VFREDOSUM.VS (ordered sum)
			fp_sew_dispatch(cpu, [&] (auto tag) {
				using F = decltype(tag);
				reduction_loop<F>(cpu, vd, vs1, vs2, vm, [] (F acc, F x) -> F {
					return acc + x;
				});
			});
			return;
		case 0b000100: // VFMIN.VV (number-aware: NaN returns the other operand)
		case 0b000110: // VFMAX.VV
			fp_sew_dispatch(cpu, [&] (auto tag) {
				using F = decltype(tag);
				const bool is_min = vi.OPVV.funct6 == 0b000100;
				vector_element_loop<F>(cpu, vd, vm, [&] (uint64_t i) {
					const F a = element_at<F>(rvv, vs2, i), b = element_at<F>(rvv, vs1, i);
					if (std::isnan(a)) return b;
					if (std::isnan(b)) return a;
					return is_min ? (a < b ? a : b) : (a > b ? a : b);
				});
			});
			return;
		case 0b000101: // VFREDMIN.VS
		case 0b000111: // VFREDMAX.VS
			fp_sew_dispatch(cpu, [&] (auto tag) {
				using F = decltype(tag);
				const bool is_min = vi.OPVV.funct6 == 0b000101;
				reduction_loop<F>(cpu, vd, vs1, vs2, vm, [is_min] (F acc, F x) -> F {
					if (std::isnan(acc)) return x;
					if (std::isnan(x)) return acc;
					return is_min ? (acc < x ? acc : x) : (acc > x ? acc : x);
				});
			});
			return;
		case 0b001000: // VFSGNJ.VV
		case 0b001001: // VFSGNJN.VV
		case 0b001010: // VFSGNJX.VV
			fp_sew_dispatch(cpu, [&] (auto tag) {
				using F = decltype(tag);
				using B = std::conditional_t<sizeof(F) == 4, uint32_t, uint64_t>;
				constexpr B sign_bit = B(1) << (8 * sizeof(B) - 1);
				const unsigned funct6 = vi.OPVV.funct6;
				vector_element_loop<F>(cpu, vd, vm, [&] (uint64_t i) {
					B a = (B)element_at<F>(rvv, vs2, i);
					const B b = (B)element_at<F>(rvv, vs1, i);
					if (funct6 == 0b001000) a = (a & ~sign_bit) | (b & sign_bit);
					else if (funct6 == 0b001001) a = (a & ~sign_bit) | ((~b) & sign_bit);
					else a = a ^ (b & sign_bit);
					return (F)a;
				});
			});
			return;
		case 0b010000: // VFMV.F.S: f[rd] = vs2[0]
			if (vs1 == 0b00000 && vm) {
				fp_sew_dispatch(cpu, [&] (auto tag) {
					using F = decltype(tag);
					auto& fl = cpu.registers().getfl(vd);
					if constexpr (sizeof(F) == 4)
						fl.set_float(element_at<float>(rvv, vs2, 0));
					else
						fl.set_double(element_at<double>(rvv, vs2, 0));
				});
				return;
			}
			break;
		case 0b010011: // VFSQRT.V (vs1=0), VFCLASS.V (vs1=10000)
			if (vs1 == 0b00000) {
				fp_sew_dispatch(cpu, [&] (auto tag) {
					using F = decltype(tag);
					vector_element_loop<F>(cpu, vd, vm, [&] (uint64_t i) {
						return std::sqrt(element_at<F>(rvv, vs2, i));
					});
				});
				return;
			} else if (vs1 == 0b10000) {
				fp_sew_dispatch(cpu, [&] (auto tag) {
					using F = decltype(tag);
					mask_dest_loop(cpu, vd, vm, [&] (uint64_t i) {
						return fp_class_mask(element_at<F>(rvv, vs2, i));
					});
				});
				return;
			}
			break;
		case 0b010010: // VFCVT (vs1 selects the conversion)
			// Widening/narrowing forms (vs1 >= 8) take SEW/2 or 2*SEW
			// sources and bypass the SEW float dispatch.
			if (vs1 >= 8) {
				fp_widen_narrow(cpu, vd, vs2, vm, vs1);
				return;
			}
			fp_sew_dispatch(cpu, [&] (auto tag) {
				using F = decltype(tag);
				using B = std::conditional_t<sizeof(F) == 4, uint32_t, uint64_t>;
				using SB = std::conditional_t<sizeof(F) == 4, int32_t, int64_t>;
				switch (vs1) {
				case 0b00000: // VFCVT.XU.F.V (current rounding mode)
					vector_element_loop<B>(cpu, vd, vm, [&] (uint64_t i) {
						return f2i_sat<B>(element_at<F>(rvv, vs2, i), false);
					});
					return;
				case 0b00001: // VFCVT.X.F.V (current rounding mode)
					vector_element_loop<B>(cpu, vd, vm, [&] (uint64_t i) {
						return (B)f2i_sat<SB>(element_at<F>(rvv, vs2, i), false);
					});
					return;
				case 0b00110: // VFCVT.RTZ.XU.F.V
					vector_element_loop<B>(cpu, vd, vm, [&] (uint64_t i) {
						return f2i_sat<B>(element_at<F>(rvv, vs2, i), true);
					});
					return;
				case 0b00111: // VFCVT.RTZ.X.F.V
					vector_element_loop<B>(cpu, vd, vm, [&] (uint64_t i) {
						return (B)f2i_sat<SB>(element_at<F>(rvv, vs2, i), true);
					});
					return;
				case 0b00010: // VFCVT.F.XU.V
					vector_element_loop<F>(cpu, vd, vm, [&] (uint64_t i) {
						return (F)element_at<B>(rvv, vs2, i);
					});
					return;
				case 0b00011: // VFCVT.F.X.V
					vector_element_loop<F>(cpu, vd, vm, [&] (uint64_t i) {
						return (F)(SB)element_at<B>(rvv, vs2, i);
					});
					return;
				default:
					break;
				}
				cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION);
			});
			return;
		case 0b011000: // VMFEQ.VV
		case 0b011001: // VMFLE.VV
		case 0b011011: // VMFLT.VV
		case 0b011100: { // VMFNE.VV (false when either operand is NaN)
			fp_sew_dispatch(cpu, [&] (auto tag) {
				using F = decltype(tag);
				const unsigned funct6 = vi.OPVV.funct6;
				mask_dest_loop(cpu, vd, vm, [&] (uint64_t i) {
					const F a = element_at<F>(rvv, vs2, i), b = element_at<F>(rvv, vs1, i);
					switch (funct6) {
						case 0b011000: return a == b;
						case 0b011001: return a <= b;
						case 0b011011: return a <  b;
						default:        return a != b;
					}
				});
			});
			return; }
		case 0b100000: // VFDIV.VV
			fp_sew_dispatch(cpu, [&] (auto tag) {
				using F = decltype(tag);
				vector_element_loop<F>(cpu, vd, vm, [&] (uint64_t i) {
					return element_at<F>(rvv, vs2, i) / element_at<F>(rvv, vs1, i);
				});
			});
			return;
		case 0b100100: // VFMUL.VV
			fp_sew_dispatch(cpu, [&] (auto tag) {
				using F = decltype(tag);
				vector_element_loop<F>(cpu, vd, vm, [&] (uint64_t i) {
					return element_at<F>(rvv, vs2, i) * element_at<F>(rvv, vs1, i);
				});
			});
			return;
		case 0b101000: // VFMADD.VV:  vd = +(vs1 * vd) + vs2
		case 0b101001: // VFNMADD.VV: vd = -(vs1 * vd) - vs2
		case 0b101010: // VFMSUB.VV:  vd = +(vs1 * vd) - vs2
		case 0b101011: // VFNMSUB.VV: vd = -(vs1 * vd) + vs2
		case 0b101100: // VFMACC.VV:  vd = +(vs1 * vs2) + vd
		case 0b101101: // VFNMACC.VV: vd = -(vs1 * vs2) - vd
		case 0b101110: // VFMSAC.VV:  vd = +(vs1 * vs2) - vd
		case 0b101111: // VFNMSAC.VV: vd = -(vs1 * vs2) + vd
			fp_sew_dispatch(cpu, [&] (auto tag) {
				using F = decltype(tag);
				const unsigned funct6 = vi.OPVV.funct6;
				vector_element_loop<F>(cpu, vd, vm, [&] (uint64_t i) {
					const F a = element_at<F>(rvv, vs1, i);
					const F b = element_at<F>(rvv, vs2, i);
					const F c = element_at<F>(rvv, vd, i);
					switch (funct6) {
						case 0b101000: return a * c + b;
						case 0b101001: return -(a * c) - b;
						case 0b101010: return a * c - b;
						case 0b101011: return -(a * c) + b;
						case 0b101100: return a * b + c;
						case 0b101101: return -(a * b) - c;
						case 0b101110: return a * b - c;
						default:        return -(a * b) + c;
					}
				});
			});
			return;
		} // switch
		cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		const rv32v_instruction vi { instr };
		return snprintf(buffer, len, "%s.VV v%u, v%u, v%u",
						OPFNAMES[vi.OPVV.funct6],
						vi.OPVV.vd, vi.OPVV.vs2, vi.OPVV.vs1);
	});

	/* OPFVF: vd = vs2 OP f[rs1] */
	VECTOR_INSTR(VOPF_VF,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32v_instruction vi { instr };
		require_valid_vtype(cpu);
		auto& rvv = cpu.registers().rvv();
		const unsigned vd = vi.OPVV.vd, vs1 = vi.OPVV.vs1, vs2 = vi.OPVV.vs2;
		const bool vm = vi.OPVV.vm;
		check_register_group(cpu, vd);
		check_register_group(cpu, vs2);

		float  scalar_f = 0.0f;
		double scalar_d = 0.0;
		switch (rvv.sew()) {
			case 32: scalar_f = cpu.registers().getfl(vs1).f32[0]; break;
			case 64: scalar_d = cpu.registers().getfl(vs1).f64;    break;
			default: cpu.trigger_exception(ILLEGAL_OPCODE);
		}

		switch (vi.OPVV.funct6) {
		case 0b000000: // VFADD.VF
			fp_sew_dispatch(cpu, [&] (auto tag) {
				using F = decltype(tag);
				const F s = sizeof(F) == 4 ? (F)scalar_f : (F)scalar_d;
				vector_element_loop<F>(cpu, vd, vm, [&] (uint64_t i) {
					return element_at<F>(rvv, vs2, i) + s;
				});
			});
			return;
		case 0b000010: // VFSUB.VF: vd = vs2 - f[rs1]
			fp_sew_dispatch(cpu, [&] (auto tag) {
				using F = decltype(tag);
				const F s = sizeof(F) == 4 ? (F)scalar_f : (F)scalar_d;
				vector_element_loop<F>(cpu, vd, vm, [&] (uint64_t i) {
					return element_at<F>(rvv, vs2, i) - s;
				});
			});
			return;
		case 0b100111: // VFRSUB.VF: vd = f[rs1] - vs2
			fp_sew_dispatch(cpu, [&] (auto tag) {
				using F = decltype(tag);
				const F s = sizeof(F) == 4 ? (F)scalar_f : (F)scalar_d;
				vector_element_loop<F>(cpu, vd, vm, [&] (uint64_t i) {
					return s - element_at<F>(rvv, vs2, i);
				});
			});
			return;
		case 0b000100: // VFMIN.VF
		case 0b000110: // VFMAX.VF
			fp_sew_dispatch(cpu, [&] (auto tag) {
				using F = decltype(tag);
				const F s = sizeof(F) == 4 ? (F)scalar_f : (F)scalar_d;
				const bool is_min = vi.OPVV.funct6 == 0b000100;
				vector_element_loop<F>(cpu, vd, vm, [&] (uint64_t i) {
					const F a = element_at<F>(rvv, vs2, i);
					if (std::isnan(a)) return s;
					if (std::isnan(s)) return a;
					return is_min ? (a < s ? a : s) : (a > s ? a : s);
				});
			});
			return;
		case 0b001000: // VFSGNJ.VF
		case 0b001001: // VFSGNJN.VF
		case 0b001010: // VFSGNJX.VF
			fp_sew_dispatch(cpu, [&] (auto tag) {
				using F = decltype(tag);
				using B = std::conditional_t<sizeof(F) == 4, uint32_t, uint64_t>;
				constexpr B sign_bit = B(1) << (8 * sizeof(B) - 1);
				const F sf = sizeof(F) == 4 ? (F)scalar_f : (F)scalar_d;
				const unsigned funct6 = vi.OPVV.funct6;
				vector_element_loop<F>(cpu, vd, vm, [&] (uint64_t i) {
					B a = (B)element_at<F>(rvv, vs2, i);
					const B b = (B)sf;
					if (funct6 == 0b001000) a = (a & ~sign_bit) | (b & sign_bit);
					else if (funct6 == 0b001001) a = (a & ~sign_bit) | ((~b) & sign_bit);
					else a = a ^ (b & sign_bit);
					return (F)a;
				});
			});
			return;
		case 0b001110: { // VFSLIDE1UP.VF
			const uint64_t vl = rvv.vl();
			if (vl > 0) {
				fp_sew_dispatch(cpu, [&] (auto tag) {
					using F = decltype(tag);
					const F s = sizeof(F) == 4 ? (F)scalar_f : (F)scalar_d;
					for (uint64_t i = vl - 1; i > 0; i--) {
						if (element_active(rvv, vm, i))
							element_at<F>(rvv, vd, i) = element_at<F>(rvv, vs2, i - 1);
					}
					if (element_active(rvv, vm, 0))
						element_at<F>(rvv, vd, 0) = s;
				});
			}
			return; }
		case 0b001111: { // VFSLIDE1DOWN.VF
			const uint64_t vl = rvv.vl();
			fp_sew_dispatch(cpu, [&] (auto tag) {
				using F = decltype(tag);
				const F s = sizeof(F) == 4 ? (F)scalar_f : (F)scalar_d;
				for (uint64_t i = 0; i + 1 < vl; i++) {
					if (element_active(rvv, vm, i))
						element_at<F>(rvv, vd, i) = element_at<F>(rvv, vs2, i + 1);
				}
				if (vl > 0 && element_active(rvv, vm, vl - 1))
					element_at<F>(rvv, vd, vl - 1) = s;
			});
			return; }
		case 0b010000: { // VFMV.S.F: vd[0] = f[rs1]
			if (rvv.vl() > 0) {
				fp_sew_dispatch(cpu, [&] (auto tag) {
					using F = decltype(tag);
					element_at<F>(rvv, vd, 0) =
						sizeof(F) == 4 ? (F)scalar_f : (F)scalar_d;
				});
			}
			return; }
		case 0b010111: // VFMERGE.VFM (vm=0) / VFMV.V.F (vm=1)
			// VFMERGE has no masked form (see VOPI_VV).
			if (!vm) {
				fp_sew_dispatch(cpu, [&] (auto tag) {
					using F = decltype(tag);
					const F s = sizeof(F) == 4 ? (F)scalar_f : (F)scalar_d;
					vector_element_loop<F>(cpu, vd, true, [&] (uint64_t i) {
						return rvv.get(0).mask(i) ? s : element_at<F>(rvv, vs2, i);
					});
				});
			} else {
				fp_sew_dispatch(cpu, [&] (auto tag) {
					using F = decltype(tag);
					const F s = sizeof(F) == 4 ? (F)scalar_f : (F)scalar_d;
					vector_element_loop<F>(cpu, vd, true, [&] (uint64_t) {
						return s;
					});
				});
			}
			return;
		case 0b011000: // VMFEQ.VF
		case 0b011001: // VMFLE.VF
		case 0b011011: // VMFLT.VF
		case 0b011100: // VMFNE.VF
		case 0b011101: // VMFGT.VF: vs2 > f[rs1]
		case 0b011111: // VMFGE.VF: vs2 >= f[rs1]
			fp_sew_dispatch(cpu, [&] (auto tag) {
				using F = decltype(tag);
				const F s = sizeof(F) == 4 ? (F)scalar_f : (F)scalar_d;
				const unsigned funct6 = vi.OPVV.funct6;
				mask_dest_loop(cpu, vd, vm, [&] (uint64_t i) {
					const F a = element_at<F>(rvv, vs2, i);
					switch (funct6) {
						case 0b011000: return a == s;
						case 0b011001: return a <= s;
						case 0b011011: return a <  s;
						case 0b011100: return a != s;
						case 0b011101: return a >  s;
						default:        return a >= s;
					}
				});
			});
			return;
		case 0b100000: // VFDIV.VF: vd = vs2 / f[rs1]
			fp_sew_dispatch(cpu, [&] (auto tag) {
				using F = decltype(tag);
				const F s = sizeof(F) == 4 ? (F)scalar_f : (F)scalar_d;
				vector_element_loop<F>(cpu, vd, vm, [&] (uint64_t i) {
					return element_at<F>(rvv, vs2, i) / s;
				});
			});
			return;
		case 0b100001: // VFRDIV.VF: vd = f[rs1] / vs2
			fp_sew_dispatch(cpu, [&] (auto tag) {
				using F = decltype(tag);
				const F s = sizeof(F) == 4 ? (F)scalar_f : (F)scalar_d;
				vector_element_loop<F>(cpu, vd, vm, [&] (uint64_t i) {
					return s / element_at<F>(rvv, vs2, i);
				});
			});
			return;
		case 0b100100: // VFMUL.VF
			fp_sew_dispatch(cpu, [&] (auto tag) {
				using F = decltype(tag);
				const F s = sizeof(F) == 4 ? (F)scalar_f : (F)scalar_d;
				vector_element_loop<F>(cpu, vd, vm, [&] (uint64_t i) {
					return element_at<F>(rvv, vs2, i) * s;
				});
			});
			return;
		case 0b101000: // VFMADD.VF
		case 0b101001: // VFNMADD.VF
		case 0b101010: // VFMSUB.VF
		case 0b101011: // VFNMSUB.VF
		case 0b101100: // VFMACC.VF
		case 0b101101: // VFNMACC.VF
		case 0b101110: // VFMSAC.VF
		case 0b101111: // VFNMSAC.VF
			fp_sew_dispatch(cpu, [&] (auto tag) {
				using F = decltype(tag);
				const F s = sizeof(F) == 4 ? (F)scalar_f : (F)scalar_d;
				const unsigned funct6 = vi.OPVV.funct6;
				vector_element_loop<F>(cpu, vd, vm, [&] (uint64_t i) {
					const F b = element_at<F>(rvv, vs2, i);
					const F c = element_at<F>(rvv, vd, i);
					switch (funct6) {
						case 0b101000: return s * c + b;
						case 0b101001: return -(s * c) - b;
						case 0b101010: return s * c - b;
						case 0b101011: return -(s * c) + b;
						case 0b101100: return s * b + c;
						case 0b101101: return -(s * b) - c;
						case 0b101110: return s * b - c;
						default:        return -(s * b) + c;
					}
				});
			});
			return;
		} // switch
		cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION);
	},
	[] (char* buffer, size_t len, auto&, rv32i_instruction instr) RVPRINTR_ATTR {
		const rv32v_instruction vi { instr };
		return snprintf(buffer, len, "%s.VF v%u, v%u, f%u",
						OPFNAMES[vi.OPVV.funct6],
						vi.OPVV.vd, vi.OPVV.vs2, vi.OPVV.vs1);
	});
} // riscv
