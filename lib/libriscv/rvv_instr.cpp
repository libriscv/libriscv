#include "rvv.hpp"
#include "fp16.hpp"
#include "instr_helpers.hpp"
#include "internal_common.hpp"
#include "rvv_printer.hpp"
#include <cmath>
#include <cstdint>
#include <type_traits>

#define VECTOR_PRINTER(family, accepted) \
	[] (char* buffer, size_t len, auto&, riscv::rv32i_instruction instr) RVPRINTR_ATTR { \
		return riscv::RVVDISASM::expect(buffer, len, instr, family, accepted); \
	}
#define VSETVL_PRINTER(family, accepted) \
	[] (char* buffer, size_t len, auto&, riscv::rv32i_instruction instr) RVPRINTR_ATTR { \
		return riscv::RVVDISASM::expect_config(buffer, len, instr, family, accepted); \
	}
#define VECTOR_MEM_PRINTER(is_store) \
	[] (char* buffer, size_t len, auto&, riscv::rv32i_instruction instr) RVPRINTR_ATTR { \
		return riscv::RVVDISASM::op_memory(buffer, len, instr, is_store); \
	}

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

	// Registers in a group. A fractional EMUL still occupies one.
	constexpr unsigned group_size(const int emul) noexcept
	{
		return emul > 0 ? (1u << emul) : 1u;
	}

	// A register group must be aligned to its own size and fit in v0-v31.
	template <typename CPU_t>
	RISCV_ALWAYS_INLINE void check_group(CPU_t& cpu, const unsigned vreg, const unsigned regs)
	{
		if (UNLIKELY(regs > 1 && ((vreg % regs) != 0 || vreg + regs > 32)))
			cpu.trigger_exception(ILLEGAL_OPCODE);
	}

	// A register group operand must be aligned and in bounds when LMUL > 1.
	template <typename CPU_t>
	RISCV_ALWAYS_INLINE void check_register_group(CPU_t& cpu, const unsigned vreg)
	{
		check_group(cpu, vreg, cpu.registers().rvv().group_regs());
	}

	// The same, for the 2*SEW operand of a widening or narrowing
	// instruction. Doubling EMUL is a step along the LMUL scale, not a
	// doubling of the register count: at LMUL=1/2 the wide group is still
	// a single register, and so still has no alignment to meet.
	template <typename CPU_t>
	RISCV_ALWAYS_INLINE void check_wide_group(CPU_t& cpu, const unsigned vreg)
	{
		const int emul = cpu.registers().rvv().lmul_shift() + 1;
		// Eight registers is as large as a group gets, so there is nowhere
		// to put a widened result at LMUL=8.
		if (UNLIKELY(emul > 3))
			cpu.trigger_exception(ILLEGAL_OPCODE);
		check_group(cpu, vreg, group_size(emul));
	}

	// The EMUL of a register group holding elements of eew bytes, as a
	// log2. Traps when the encoding asks for a group the architecture does
	// not have (EMUL outside 1/8 .. 8).
	template <typename CPU_t>
	static int emul_shift_for(CPU_t& cpu, const unsigned eew_bytes)
	{
		auto& rvv = cpu.registers().rvv();
		const int emul = rvv.lmul_shift()
			+ (int)bits_of(eew_bytes) - (int)bits_of(rvv.sew() / 8);
		if (UNLIKELY(emul < -3 || emul > 3))
			cpu.trigger_exception(ILLEGAL_OPCODE);
		return emul;
	}

	// Element *i* of the register group starting at *vreg*. T is the
	// SEW-sized element type.
	template <typename T, typename RVV_t>
	RISCV_ALWAYS_INLINE T& element_at(RVV_t& rvv, const unsigned vreg, const uint64_t i)
	{
		constexpr unsigned per_reg = VectorLane::size() / sizeof(T);
		constexpr uint64_t elements = 32ull * per_reg;
		auto* flat = reinterpret_cast<T*>(&rvv.get(0));
		return flat[(uint64_t(vreg) * per_reg + i) & (elements - 1)];
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

	template <typename CPU_t, typename F>
	RISCV_ALWAYS_INLINE static void fp_sew_dispatch_inline(CPU_t& cpu, F&& body)
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
		if (LIKELY(vm)) {
			// One whole register at LMUL=1 -- what a vectorised loop runs at
			// for all but its last pass. Spelling the trip count out as a
			// constant is what pays here: the host compiler can then see that
			// no index can leave the group, drop the wrap in element_at(),
			// and do the whole register in a SIMD instruction or two.
			constexpr unsigned per_reg = VectorLane::size() / sizeof(T);
			if (LIKELY(vl == per_reg)) {
				for (unsigned i = 0; i < per_reg; i++) {
					element_at<T>(rvv, vd, i) = compute(i);
				}
				return;
			}
			for (uint64_t i = 0; i < vl; i++) {
				element_at<T>(rvv, vd, i) = compute(i);
			}
			return;
		}
		const auto& mask = rvv.get(0);
		for (uint64_t i = 0; i < vl; i++) {
			if (mask.mask(i)) {
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

	// The next wider integer type. ELEN is 64, so there is deliberately no
	// entry for a 64-bit source: a widening instruction at SEW=64 has no
	// destination element to write, and is reserved.
	template <typename T> struct widen_type;
	template <> struct widen_type<int8_t>   { using type = int16_t;  };
	template <> struct widen_type<int16_t>  { using type = int32_t;  };
	template <> struct widen_type<int32_t>  { using type = int64_t;  };
	template <> struct widen_type<uint8_t>  { using type = uint16_t; };
	template <> struct widen_type<uint16_t> { using type = uint32_t; };
	template <> struct widen_type<uint32_t> { using type = uint64_t; };
	template <typename T> using widen_t = typename widen_type<T>::type;

	// Dispatch a lambda over the SEWs a widening or narrowing integer
	// instruction is defined for.
	template <typename CPU_t, typename F>
	static void widen_sew_dispatch(CPU_t& cpu, F&& body)
	{
		switch (cpu.registers().rvv().sew()) {
			case 8:  body(int8_t{});  break;
			case 16: body(int16_t{}); break;
			case 32: body(int32_t{}); break;
			default: cpu.trigger_exception(ILLEGAL_OPCODE);
		}
	}

	// Apply vxrm rounding to a pre-shifted value given the guard bit (lsb)
	// and sticky bit (rest). Used directly when 2*SEW is unavailable.
	template <typename T>
	static T round_adjust(const T shifted, const bool lsb, const bool rest,
		const unsigned vxrm)
	{
		switch (vxrm) {
		case 0: return T(shifted + lsb);                         // rnu
		case 1: // rne: a tie rounds to the even result
			return T(shifted + ((lsb && (rest || (shifted & 1))) ? 1 : 0));
		case 2: return shifted;                                  // rdn
		default: // rod: force an odd result unless the shift was exact
			return (lsb || rest) ? T(shifted | 1) : shifted;
		}
	}

	// Rounding right shift shared by vssrl/vssra, vnclip and vsmul.
	template <typename T, typename U = std::make_unsigned_t<T>>
	static T roundoff(const T value, const unsigned shift, const unsigned vxrm)
	{
		if (shift == 0)
			return value;
		const T shifted = T(value >> shift);
		const U bits = (U)value;
		// Guard and sticky bits for vxrm rounding.
		const bool lsb = (U(bits >> (shift - 1)) & 1) != 0;
		const bool rest = shift > 1
			&& (bits & U((U(1) << (shift - 1)) - 1)) != 0;
		return round_adjust<T>(shifted, lsb, rest, vxrm);
	}

	// Upper SEW bits of a 2*SEW product (vmulh/vmulhu/vmulhsu).
	// SEW<64 uses a 64-bit intermediate; SEW=64 delegates to the scalar MULH helpers.
	template <typename E>
	static E mul_high(const E a, const E b, const bool a_signed, const bool b_signed)
	{
		using U = std::make_unsigned_t<E>;
		if constexpr (sizeof(E) < sizeof(int64_t)) {
			// Sign/zero-extend to 64 bits; unsigned multiply is fine since
			// only the upper SEW bits are kept.
			const uint64_t wa = a_signed ? (uint64_t)(int64_t)a : (uint64_t)(U)a;
			const uint64_t wb = b_signed ? (uint64_t)(int64_t)b : (uint64_t)(U)b;
			return (E)((wa * wb) >> (8 * sizeof(E)));
		} else {
			if (a_signed && b_signed)
				return (E)mulhi64((uint64_t)a, (uint64_t)b);
			else if (a_signed)
				return (E)mulhsu64((uint64_t)a, (uint64_t)b);
			return (E)mulhu64((uint64_t)a, (uint64_t)b);
		}
	}

	// Clamp a wide value into the range of E, latching vxsat when it does
	// not fit. This is what makes vnclip a clip rather than a truncation.
	template <typename E, typename WT, typename RVV_t>
	static E saturate_to(RVV_t& rvv, const WT value)
	{
		constexpr WT lo = std::is_signed_v<E>
			? WT(-(WT(1) << (8 * sizeof(E) - 1))) : WT(0);
		constexpr WT hi = std::is_signed_v<E>
			? WT((WT(1) << (8 * sizeof(E) - 1)) - 1)
			: WT((WT(1) << (8 * sizeof(E))) - 1);
		if (value < lo) { rvv.set_vxsat(true); return (E)lo; }
		if (value > hi) { rvv.set_vxsat(true); return (E)hi; }
		return (E)value;
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

	// ---- binary16, for Zvfhmin ------------------------------------------
	//
	static float f16_to_f32(const uint16_t h) noexcept { return fp16::to_f32(h); }
	static uint16_t f32_to_f16(const float value) noexcept { return fp16::from_f32(value); }

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

	/**
	 * The vfwcvt (sel 8-15) and vfncvt (sel 16-23) conversions.
	 *
	 * In both families SEW names the *narrow* side: a widening conversion
	 * reads SEW and writes 2*SEW, a narrowing one reads 2*SEW and writes
	 * SEW. Half precision appears here as a bit pattern in a 16-bit
	 * element rather than as a type, because Zvfhmin supplies the
	 * conversions to and from binary16 without any arithmetic on it.
	 */
	template <typename CPU_t>
	static void fp_widen_narrow(CPU_t& cpu, const unsigned vd,
		const unsigned vs2, const bool vm, const unsigned sel)
	{
		auto& rvv = cpu.registers().rvv();
		const unsigned frm = cpu.registers().fcsr().frm;

		if (sel < 16) {
			// Widening: SEW is the source, 2*SEW the destination.
			switch (rvv.sew()) {
			case 16: // binary16 or a 16-bit integer, out to 32 bits
				switch (sel) {
				case 8: vector_element_loop<uint32_t>(cpu, vd, vm, [&] (uint64_t i) {
						return f2i_frm_sat<uint32_t>(
							f16_to_f32(element_at<uint16_t>(rvv, vs2, i)), frm); });
					return;
				case 9: vector_element_loop<int32_t>(cpu, vd, vm, [&] (uint64_t i) {
						return f2i_frm_sat<int32_t>(
							f16_to_f32(element_at<uint16_t>(rvv, vs2, i)), frm); });
					return;
				case 10: vector_element_loop<float>(cpu, vd, vm, [&] (uint64_t i) {
						return (float)element_at<uint16_t>(rvv, vs2, i); });
					return;
				case 11: vector_element_loop<float>(cpu, vd, vm, [&] (uint64_t i) {
						return (float)element_at<int16_t>(rvv, vs2, i); });
					return;
				case 12: vector_element_loop<float>(cpu, vd, vm, [&] (uint64_t i) {
						return f16_to_f32(element_at<uint16_t>(rvv, vs2, i)); });
					return;
				case 14: vector_element_loop<uint32_t>(cpu, vd, vm, [&] (uint64_t i) {
						return f2i_sat<uint32_t>(
							f16_to_f32(element_at<uint16_t>(rvv, vs2, i)), true); });
					return;
				case 15: vector_element_loop<int32_t>(cpu, vd, vm, [&] (uint64_t i) {
						return f2i_sat<int32_t>(
							f16_to_f32(element_at<uint16_t>(rvv, vs2, i)), true); });
					return;
				}
				break;
			case 32: // single precision or a 32-bit integer, out to 64 bits
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
			// SEW=8 would widen into binary16, which needs Zvfh.
			cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION);
		}

		// Narrowing: 2*SEW is the source, SEW the destination.
		switch (rvv.sew()) {
		case 16: // single precision or a 32-bit integer, down to 16 bits
			switch (sel) {
			case 16: vector_element_loop<uint16_t>(cpu, vd, vm, [&] (uint64_t i) {
					return f2i_frm_sat<uint16_t>(element_at<float>(rvv, vs2, i), frm); });
				return;
			case 17: vector_element_loop<int16_t>(cpu, vd, vm, [&] (uint64_t i) {
					return f2i_frm_sat<int16_t>(element_at<float>(rvv, vs2, i), frm); });
				return;
			case 18: vector_element_loop<uint16_t>(cpu, vd, vm, [&] (uint64_t i) {
					return f32_to_f16((float)element_at<uint32_t>(rvv, vs2, i)); });
				return;
			case 19: vector_element_loop<uint16_t>(cpu, vd, vm, [&] (uint64_t i) {
					return f32_to_f16((float)element_at<int32_t>(rvv, vs2, i)); });
				return;
			case 20: vector_element_loop<uint16_t>(cpu, vd, vm, [&] (uint64_t i) {
					return f32_to_f16(element_at<float>(rvv, vs2, i)); });
				return;
			case 21: // vfncvt.rod.f.f.w: round to odd
				vector_element_loop<uint16_t>(cpu, vd, vm, [&] (uint64_t i) {
					const float v = element_at<float>(rvv, vs2, i);
					uint16_t r = f32_to_f16(v);
					// Round-to-odd exists so a second narrowing step
					// cannot double-round; it only bites when the first
					// one was inexact.
					if (f16_to_f32(r) != v)
						r |= 1;
					return r;
				});
				return;
			case 22: vector_element_loop<uint16_t>(cpu, vd, vm, [&] (uint64_t i) {
					return f2i_sat<uint16_t>(element_at<float>(rvv, vs2, i), true); });
				return;
			case 23: vector_element_loop<int16_t>(cpu, vd, vm, [&] (uint64_t i) {
					return f2i_sat<int16_t>(element_at<float>(rvv, vs2, i), true); });
				return;
			}
			break;
		case 32: // double precision or a 64-bit integer, down to 32 bits
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
				vector_element_loop<float>(cpu, vd, vm, [&] (uint64_t i) {
					const double v = element_at<double>(rvv, vs2, i);
					float r = (float)v;
					if ((double)r != v) {
						uint32_t b;
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

	/**
	 * The integer code points whose second source is read the same way in
	 * all three forms, but whose arithmetic is not plain wraparound: the
	 * fixed-point multiply, the two scaling shifts, and the four narrowing
	 * ones. Returns false for a funct6 that is none of them, so the caller
	 * can carry on with its own switch.
	 *
	 * `scalar` is the .vx or .vi operand, already extended; the .vv form
	 * passes is_vv and the operand comes out of vs1 per element instead.
	 * The shifts want it unsigned and the multiply wants it signed, which
	 * is why it arrives as a bit pattern rather than a value.
	 */
	template <typename CPU_t>
	static bool integer_fixedpoint(CPU_t& cpu, const rv32v_instruction& vi,
		const bool is_vv, const uint64_t scalar)
	{
		auto& rvv = cpu.registers().rvv();
		const unsigned vd = vi.OPVV.vd, vs1 = vi.OPVV.vs1, vs2 = vi.OPVV.vs2;
		const bool vm = vi.OPVV.vm;
		const unsigned funct6 = vi.OPVV.funct6;
		const unsigned vxrm = rvv.vxrm();

		switch (funct6) {
		case 0b100111: // VSMUL: signed multiply of two fractions
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				constexpr unsigned bits = 8 * sizeof(E);
				// Q(SEW-1) fixed-point multiply; only emin*emin saturates.
				constexpr E emin = E(std::make_unsigned_t<E>(1) << (bits - 1));
				vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t i) {
					const E a = element_at<E>(rvv, vs2, i);
					const E b = is_vv ? element_at<E>(rvv, vs1, i) : (E)scalar;
					if (a == emin && b == emin) {
						rvv.set_vxsat(true);
						return (E)~emin;
					}
					if constexpr (sizeof(E) < sizeof(int64_t)) {
						const int64_t prod = (int64_t)a * (int64_t)b;
						return (E)roundoff<int64_t>(prod, bits - 1, vxrm);
					} else {
						// SEW=64: no 2*SEW type. Reconstruct the Q(SEW-1)
						// fixed-point result from hi:lo and round with vxrm.
						const uint64_t lo = (uint64_t)a * (uint64_t)b;
						const uint64_t hi = (uint64_t)mul_high<E>(a, b, true, true);
						const E shifted = (E)((hi << 1) | (lo >> 63));
						const bool lsb = ((lo >> 62) & 1) != 0;
						const bool rest = (lo & ((uint64_t(1) << 62) - 1)) != 0;
						return round_adjust<E>(shifted, lsb, rest, vxrm);
					}
				});
			});
			return true;
		case 0b101010: // VSSRL: scaling shift right, logical
		case 0b101011: // VSSRA: scaling shift right, arithmetic
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				using U = std::make_unsigned_t<E>;
				constexpr unsigned shamt_mask = 8 * sizeof(E) - 1;
				const bool arith = funct6 == 0b101011;
				vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t i) {
					const unsigned sh = unsigned(is_vv
						? (U)element_at<E>(rvv, vs1, i) : scalar) & shamt_mask;
					const E a = element_at<E>(rvv, vs2, i);
					return arith ? roundoff<E>(a, sh, vxrm)
						: (E)roundoff<U>((U)a, sh, vxrm);
				});
			});
			return true;
		case 0b101100: // VNSRL: narrowing shift right, logical
		case 0b101101: // VNSRA: narrowing shift right, arithmetic
		case 0b101110: // VNCLIPU: narrow, round and saturate, unsigned
		case 0b101111: // VNCLIP: ... signed
			widen_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);              // signed SEW
				using U = std::make_unsigned_t<E>;
				using W = widen_t<E>;                 // signed 2*SEW
				using WU = std::make_unsigned_t<W>;
				// vs2 is the wide operand here, so it is the one that has
				// to be an aligned 2*LMUL group.
				check_wide_group(cpu, vs2);
				// The shift amount comes from a SEW-wide element even
				// though it indexes into a 2*SEW value.
				constexpr unsigned shamt_mask = 2 * 8 * sizeof(E) - 1;
				const bool sign = (funct6 & 1) != 0;  // VNSRA and VNCLIP
				// Only the two clips round and saturate. VNSRL and VNSRA
				// are plain shifts that keep the low SEW bits of what they
				// shift out, which is what makes them the narrowing cast a
				// compiler reaches for.
				const bool clip = funct6 >= 0b101110;
				vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t i) {
					const unsigned sh = unsigned(is_vv
						? (U)element_at<E>(rvv, vs1, i) : scalar) & shamt_mask;
					if (sign) {
						const W a = element_at<W>(rvv, vs2, i);
						if (!clip)
							return (E)(a >> sh);
						return saturate_to<E, W>(rvv, roundoff<W>(a, sh, vxrm));
					}
					const WU a = element_at<WU>(rvv, vs2, i);
					if (!clip)
						return (E)(a >> sh);
					return (E)saturate_to<U, WU>(rvv, roundoff<WU>(a, sh, vxrm));
				});
			});
			return true;
		}
		return false;
	}

	/**
	 * The widening integer arithmetic, which writes a 2*SEW destination
	 * group. Three shapes share the space: the add/subtract family, whose
	 * `.w` forms already have a wide vs2 and only widen the other operand;
	 * the widening multiplies; and the widening multiply-accumulates,
	 * which also read vd.
	 *
	 * The operands differ only in whether each side is sign- or
	 * zero-extended, so each code point is reduced to that pair of
	 * decisions and then run through one loop.
	 */
	template <typename CPU_t>
	static bool integer_widening(CPU_t& cpu, const rv32v_instruction& vi,
		const bool is_vv, const uint64_t scalar)
	{
		auto& rvv = cpu.registers().rvv();
		const unsigned vd = vi.OPVV.vd, vs1 = vi.OPVV.vs1, vs2 = vi.OPVV.vs2;
		const bool vm = vi.OPVV.vm;
		const unsigned funct6 = vi.OPVV.funct6;
		if (funct6 < 0b110000 || funct6 == 0b111001)
			return false;

		widen_sew_dispatch(cpu, [&] (auto tag) {
			using E = decltype(tag);              // signed SEW
			using U = std::make_unsigned_t<E>;
			using W = widen_t<E>;                 // signed 2*SEW
			using WU = std::make_unsigned_t<W>;
			check_wide_group(cpu, vd);
			if (is_vv) check_register_group(cpu, vs1);

			// The add/subtract family: bit 0 of funct6 picks signed over
			// unsigned, bit 1 subtract over add, and bit 2 the `.w` forms
			// whose vs2 is already 2*SEW.
			if (funct6 < 0b111000) {
				const bool sign = (funct6 & 1) != 0;
				const bool sub  = (funct6 & 0b10) != 0;
				const bool wide_vs2 = (funct6 & 0b100) != 0;
				if (wide_vs2) check_wide_group(cpu, vs2);
				else          check_register_group(cpu, vs2);
				vector_element_loop<W>(cpu, vd, vm, [&] (uint64_t i) {
					// vs1/rs1 is always narrow; vs2 is wide in the `.w`
					// forms and narrow otherwise.
					const E b_narrow = is_vv
						? element_at<E>(rvv, vs1, i) : (E)scalar;
					const W b = sign ? (W)b_narrow : (W)(WU)(U)b_narrow;
					const W a = wide_vs2
						? (sign ? element_at<W>(rvv, vs2, i)
						        : (W)element_at<WU>(rvv, vs2, i))
						: (sign ? (W)element_at<E>(rvv, vs2, i)
						        : (W)(WU)(U)element_at<E>(rvv, vs2, i));
					return sub ? (W)((WU)a - (WU)b) : (W)((WU)a + (WU)b);
				});
				return;
			}
			check_register_group(cpu, vs2);

			// The multiplies and multiply-accumulates. Each names which of
			// its two operands is signed; `us` is the one code point that
			// reverses the roles, and exists only in the .vx form.
			const bool accumulate = funct6 >= 0b111100;
			bool vs2_signed, other_signed;
			switch (funct6) {
			case 0b111000: vs2_signed = false; other_signed = false; break; // vwmulu
			case 0b111010: vs2_signed = true;  other_signed = false; break; // vwmulsu
			case 0b111011: vs2_signed = true;  other_signed = true;  break; // vwmul
			case 0b111100: vs2_signed = false; other_signed = false; break; // vwmaccu
			case 0b111101: vs2_signed = true;  other_signed = true;  break; // vwmacc
			case 0b111110: vs2_signed = true;  other_signed = false; break; // vwmaccus
			default:       vs2_signed = false; other_signed = true;  break; // vwmaccsu
			}
			if (funct6 == 0b111110 && is_vv) { // vwmaccus is .vx only
				cpu.trigger_exception(ILLEGAL_OPCODE);
				return;
			}
			vector_element_loop<W>(cpu, vd, vm, [&] (uint64_t i) {
				const E a_narrow = element_at<E>(rvv, vs2, i);
				const E b_narrow = is_vv
					? element_at<E>(rvv, vs1, i) : (E)scalar;
				const W a = vs2_signed
					? (W)a_narrow : (W)(WU)(U)a_narrow;
				const W b = other_signed
					? (W)b_narrow : (W)(WU)(U)b_narrow;
				const WU prod = (WU)a * (WU)b;
				if (!accumulate)
					return (W)prod;
				return (W)(prod + (WU)element_at<W>(rvv, vd, i));
			});
		});
		return true;
	}

	// vaadd/vaaddu/vasub/vasubu: exact (SEW+1)-bit sum/difference, rounded
	// back to SEW via vxrm. Operands are halved first to stay within SEW.
	template <typename CPU_t>
	static void integer_averaging(CPU_t& cpu, const rv32v_instruction& vi,
		const bool is_vv, const uint64_t scalar)
	{
		auto& rvv = cpu.registers().rvv();
		const unsigned vd = vi.OPVV.vd, vs1 = vi.OPVV.vs1, vs2 = vi.OPVV.vs2;
		const bool vm = vi.OPVV.vm;
		const unsigned funct6 = vi.OPVV.funct6;
		const unsigned vxrm = rvv.vxrm();

		int_sew_dispatch(cpu, [&] (auto tag) {
			using E = decltype(tag);
			using U = std::make_unsigned_t<E>;
			const bool sign = (funct6 & 1) != 0;   // VAADD and VASUB
			const bool sub  = (funct6 & 0b10) != 0;
			vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t i) {
				const E a = element_at<E>(rvv, vs2, i);
				const E b = is_vv ? element_at<E>(rvv, vs1, i) : (E)scalar;
				// Halve, combine, then round: a0/b0 carry the guard/sticky.
				const U ah = sign ? (U)(a >> 1) : (U)((U)a >> 1);
				const U bh = sign ? (U)(b >> 1) : (U)((U)b >> 1);
				const unsigned a0 = (unsigned)a & 1u, b0 = (unsigned)b & 1u;
				const U shifted = sub
					? U(ah - bh - (a0 < b0 ? 1u : 0u))
					: U(ah + bh + (a0 & b0));
				return (E)round_adjust<U>(shifted, a0 != b0, false, vxrm);
			});
		});
	}

	/// The integer divides, which return the RISC-V values for division by
	/// zero (all ones, and the dividend) rather than trapping, and handle
	/// the one signed overflow the same way the scalar M extension does.
	template <typename CPU_t>
	static void integer_divide(CPU_t& cpu, const rv32v_instruction& vi,
		const bool is_vv, const uint64_t scalar)
	{
		auto& rvv = cpu.registers().rvv();
		const unsigned vd = vi.OPVV.vd, vs1 = vi.OPVV.vs1, vs2 = vi.OPVV.vs2;
		const bool vm = vi.OPVV.vm;
		const unsigned funct6 = vi.OPVV.funct6;

		int_sew_dispatch(cpu, [&] (auto tag) {
			using E = decltype(tag);
			using U = std::make_unsigned_t<E>;
			constexpr E emin = E(U(1) << (8 * sizeof(E) - 1));
			const bool sign = (funct6 & 1) != 0;   // VDIV and VREM
			const bool rem  = (funct6 & 0b10) != 0;
			vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t i) {
				const E a = element_at<E>(rvv, vs2, i);   // dividend
				const E b = is_vv ? element_at<E>(rvv, vs1, i) : (E)scalar;
				if (b == 0)
					return rem ? a : (E)~E(0);
				if (sign) {
					// The most negative value divided by -1 overflows;
					// the quotient is defined to wrap and the remainder
					// to be zero.
					if (a == emin && b == (E)-1)
						return rem ? E(0) : emin;
					return rem ? (E)(a % b) : (E)(a / b);
				}
				return rem ? (E)((U)a % (U)b) : (E)((U)a / (U)b);
			});
		});
	}

	// ---- Zvbb ------------------------------------------------------------
	//
	// RVA23 mandates the vector bit-manipulation extension, and GCC emits
	// it from ordinary C, so these live with the rest of the arithmetic
	// rather than behind a switch of their own. Everything outside the
	// VXUNARY0 group sits in the OP-I space, where the code points were
	// unassigned in the base V extension.

	template <typename U> static U bits_reverse(U v) noexcept
	{
		U r = 0;
		for (unsigned b = 0; b < 8 * sizeof(U); b++) { r = U(r << 1) | (v & 1); v = U(v >> 1); }
		return r;
	}
	template <typename U> static U bytes_reverse(U v) noexcept
	{
		U r = 0;
		for (unsigned b = 0; b < sizeof(U); b++) { r = U(r << 8) | (v & 0xFF); v = U(v >> 8); }
		return r;
	}
	/// Reverse the bits within each byte, leaving the bytes where they are.
	template <typename U> static U bits_reverse_in_bytes(U v) noexcept
	{
		U r = 0;
		for (unsigned b = 0; b < sizeof(U); b++) {
			uint8_t x = (uint8_t)(v >> (8 * b)), y = 0;
			for (int k = 0; k < 8; k++) { y = (uint8_t)((y << 1) | (x & 1)); x >>= 1; }
			r |= (U)y << (8 * b);
		}
		return r;
	}
	template <typename U> static unsigned count_leading_zeros(const U v) noexcept
	{
		unsigned n = 0;
		for (int b = int(8 * sizeof(U)) - 1; b >= 0; b--) {
			if ((v >> b) & 1) break;
			n++;
		}
		return n;
	}
	template <typename U> static unsigned count_trailing_zeros(const U v) noexcept
	{
		unsigned n = 0;
		for (unsigned b = 0; b < 8 * sizeof(U); b++) {
			if ((v >> b) & 1) break;
			n++;
		}
		return n;
	}
	template <typename U> static unsigned count_ones(const U v) noexcept
	{
		unsigned n = 0;
		for (unsigned b = 0; b < 8 * sizeof(U); b++) n += unsigned((v >> b) & 1);
		return n;
	}

	/// VROL and VROR. The rotate amount is taken modulo SEW, so the .vi
	/// form's six-bit immediate reaches every distance at SEW=64.
	template <typename CPU_t>
	static void vector_rotate(CPU_t& cpu, const rv32v_instruction& vi,
		const bool is_vv, const uint64_t scalar, const bool right)
	{
		auto& rvv = cpu.registers().rvv();
		const unsigned vd = vi.OPVV.vd, vs1 = vi.OPVV.vs1, vs2 = vi.OPVV.vs2;
		const bool vm = vi.OPVV.vm;
		int_sew_dispatch(cpu, [&] (auto tag) {
			using E = decltype(tag);
			using U = std::make_unsigned_t<E>;
			constexpr unsigned bits = 8 * sizeof(E);
			vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t i) {
				const unsigned sh = unsigned(is_vv
					? (uint64_t)(U)element_at<E>(rvv, vs1, i) : scalar) & (bits - 1);
				const U a = (U)element_at<E>(rvv, vs2, i);
				if (sh == 0)
					return (E)a;
				return (E)(right ? U((a >> sh) | U(a << (bits - sh)))
				                 : U(U(a << sh) | (a >> (bits - sh))));
			});
		});
	}

	/// VWSLL: a shift left whose destination is 2*SEW, so nothing is lost
	/// off the top. The amount is masked to the *destination* width.
	template <typename CPU_t>
	static void vector_wide_shift(CPU_t& cpu, const rv32v_instruction& vi,
		const bool is_vv, const uint64_t scalar)
	{
		auto& rvv = cpu.registers().rvv();
		const unsigned vd = vi.OPVV.vd, vs1 = vi.OPVV.vs1, vs2 = vi.OPVV.vs2;
		const bool vm = vi.OPVV.vm;
		widen_sew_dispatch(cpu, [&] (auto tag) {
			using E = decltype(tag);
			using U = std::make_unsigned_t<E>;
			using W = widen_t<E>;
			using WU = std::make_unsigned_t<W>;
			check_wide_group(cpu, vd);
			check_register_group(cpu, vs2);
			constexpr unsigned bits = 8 * sizeof(W);
			vector_element_loop<W>(cpu, vd, vm, [&] (uint64_t i) {
				const unsigned sh = unsigned(is_vv
					? (uint64_t)(U)element_at<E>(rvv, vs1, i) : scalar) & (bits - 1);
				return (W)WU(WU((U)element_at<E>(rvv, vs2, i)) << sh);
			});
		});
	}

	/// The Zvbb per-element bit operations, which share the VXUNARY0 group
	/// with the integer extensions. Returns false for a source field that
	/// selects none of them.
	template <typename CPU_t>
	static bool zvbb_unary(CPU_t& cpu, const rv32v_instruction& vi, const unsigned sel)
	{
		auto& rvv = cpu.registers().rvv();
		const unsigned vd = vi.OPVV.vd, vs2 = vi.OPVV.vs2;
		const bool vm = vi.OPVV.vm;
		switch (sel) {
		case 0b01000: // VBREV8.V: reverse the bits within each byte
		case 0b01001: // VREV8.V:  reverse the bytes within each element
		case 0b01010: // VBREV.V:  reverse every bit of the element
		case 0b01100: // VCLZ.V
		case 0b01101: // VCTZ.V
		case 0b01110: // VCPOP.V
			break;
		default:
			return false;
		}
		check_register_group(cpu, vd);
		check_register_group(cpu, vs2);
		int_sew_dispatch(cpu, [&] (auto tag) {
			using E = decltype(tag);
			using U = std::make_unsigned_t<E>;
			vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t i) {
				const U a = (U)element_at<E>(rvv, vs2, i);
				switch (sel) {
					case 0b01000: return (E)bits_reverse_in_bytes(a);
					case 0b01001: return (E)bytes_reverse(a);
					case 0b01010: return (E)bits_reverse(a);
					case 0b01100: return (E)count_leading_zeros(a);
					case 0b01101: return (E)count_trailing_zeros(a);
					default:      return (E)count_ones(a);
				}
			});
		});
		return true;
	}

	/**
	 * The widening floating-point arithmetic. The base profile has no
	 * half-precision *arithmetic*, so the only widening step available is
	 * f32 to f64 and these exist at SEW=32 alone. As in the integer case,
	 * the `.w` forms already have a double-precision vs2 and widen only
	 * their other operand.
	 */
	template <typename CPU_t>
	static bool float_widening(CPU_t& cpu, const rv32v_instruction& vi,
		const bool is_vv, const float scalar)
	{
		auto& rvv = cpu.registers().rvv();
		const unsigned vd = vi.OPVV.vd, vs1 = vi.OPVV.vs1, vs2 = vi.OPVV.vs2;
		const bool vm = vi.OPVV.vm;
		const unsigned funct6 = vi.OPVV.funct6;
		if (funct6 < 0b110000)
			return false;
		if (UNLIKELY(rvv.sew() != 32))
			cpu.trigger_exception(ILLEGAL_OPCODE);
		check_register_group(cpu, vs2);
		// The reductions are the exception to the shape everything else
		// here has: their vd and vs1 are single registers holding one
		// double-precision element, not groups.
		const bool reduction = funct6 == 0b110001 || funct6 == 0b110011;
		if (!reduction) {
			check_wide_group(cpu, vd);
			if (is_vv) check_register_group(cpu, vs1);
		}

		// The second source, widened. It is the only place the .vv and .vf
		// forms differ.
		const auto second = [&] (uint64_t i) -> double {
			return is_vv ? (double)element_at<float>(rvv, vs1, i) : (double)scalar;
		};

		switch (funct6) {
		case 0b110000: // VFWADD.VV/VF
		case 0b110010: // VFWSUB.VV/VF
		case 0b110100: // VFWADD.WV/WF
		case 0b110110: { // VFWSUB.WV/WF
			const bool sub = (funct6 & 0b010) != 0;
			const bool wide_vs2 = (funct6 & 0b100) != 0;
			if (wide_vs2) check_wide_group(cpu, vs2);
			vector_element_loop<double>(cpu, vd, vm, [&] (uint64_t i) {
				const double a = wide_vs2
					? element_at<double>(rvv, vs2, i)
					: (double)element_at<float>(rvv, vs2, i);
				return sub ? a - second(i) : a + second(i);
			});
			return true; }
		case 0b111000: // VFWMUL.VV/VF
			vector_element_loop<double>(cpu, vd, vm, [&] (uint64_t i) {
				return (double)element_at<float>(rvv, vs2, i) * second(i);
			});
			return true;
		case 0b110001:   // VFWREDUSUM.VS
		case 0b110011: { // VFWREDOSUM.VS
			// The accumulator in vs1[0] is already double precision, and
			// the .vs forms have no scalar variant.
			if (!is_vv)
				break;
			const uint64_t vl = rvv.vl();
			if (vl == 0)
				return true;
			double acc = element_at<double>(rvv, vs1, 0);
			for (uint64_t i = 0; i < vl; i++) {
				if (element_active(rvv, vm, i))
					acc += (double)element_at<float>(rvv, vs2, i);
			}
			element_at<double>(rvv, vd, 0) = acc;
			return true; }
		case 0b111100: // VFWMACC:  vd = +(vs1 * vs2) + vd
		case 0b111101: // VFWNMACC: vd = -(vs1 * vs2) - vd
		case 0b111110: // VFWMSAC:  vd = +(vs1 * vs2) - vd
		case 0b111111: // VFWNMSAC: vd = -(vs1 * vs2) + vd
			vector_element_loop<double>(cpu, vd, vm, [&] (uint64_t i) {
				const double p = (double)element_at<float>(rvv, vs2, i) * second(i);
				const double c = element_at<double>(rvv, vd, i);
				switch (funct6) {
					case 0b111100: return  p + c;
					case 0b111101: return -p - c;
					case 0b111110: return  p - c;
					default:       return -p + c;
				}
			});
			return true;
		}
		return false;
	}

	/**
	 * VFRSQRT7 and VFREC7: seven-bit approximations of 1/sqrt(x) and 1/x,
	 * meant as the seed for a Newton-Raphson refinement. The spec defines
	 * them by table lookup rather than by accuracy bound, so the tables are
	 * reproduced exactly -- an "approximately right" answer here would put
	 * the refinement on a different trajectory than real hardware.
	 *
	 * Both index their table with the leading significand bits; vfrsqrt7
	 * also takes the exponent's low bit, because the halved exponent
	 * depends on its parity.
	 */
	static const uint8_t VFRSQRT7_TABLE[128] = {
		52, 51, 50, 48, 47, 46, 44, 43, 42, 41, 40, 39, 38, 36, 35, 34,
		33, 32, 31, 30, 30, 29, 28, 27, 26, 25, 24, 23, 23, 22, 21, 20,
		19, 19, 18, 17, 16, 16, 15, 14, 14, 13, 12, 12, 11, 10, 10,  9,
		 9,  8,  7,  7,  6,  6,  5,  4,  4,  3,  3,  2,  2,  1,  1,  0,
		127,125,123,121,119,118,116,114,113,111,109,108,106,105,103,102,
		100, 99, 97, 96, 95, 93, 92, 91, 90, 88, 87, 86, 85, 84, 83, 82,
		 80, 79, 78, 77, 76, 75, 74, 73, 72, 71, 70, 70, 69, 68, 67, 66,
		 65, 64, 63, 63, 62, 61, 60, 59, 59, 58, 57, 56, 56, 55, 54, 53,
	};
	static const uint8_t VFREC7_TABLE[128] = {
		127,125,123,121,119,117,116,114,112,110,109,107,105,104,102,100,
		 99, 97, 96, 94, 93, 91, 90, 88, 87, 85, 84, 83, 81, 80, 79, 77,
		 76, 75, 74, 72, 71, 70, 69, 68, 66, 65, 64, 63, 62, 61, 60, 59,
		 58, 57, 56, 55, 54, 53, 52, 51, 50, 49, 48, 47, 46, 45, 44, 43,
		 42, 41, 40, 40, 39, 38, 37, 36, 35, 35, 34, 33, 32, 31, 31, 30,
		 29, 28, 28, 27, 26, 25, 25, 24, 23, 23, 22, 21, 21, 20, 19, 19,
		 18, 17, 17, 16, 15, 15, 14, 14, 13, 12, 12, 11, 11, 10,  9,  9,
		  8,  8,  7,  7,  6,  5,  5,  4,  4,  3,  3,  2,  2,  1,  1,  0,
	};

	/// The IEEE fields of an F, as the two estimates need to take them
	/// apart and put them back together.
	template <typename F> struct fp_traits;
	template <> struct fp_traits<float> {
		using bits_t = uint32_t;
		static constexpr unsigned sig_bits = 23;
		static constexpr unsigned exp_bits = 8;
		static constexpr int bias = 127;
	};
	template <> struct fp_traits<double> {
		using bits_t = uint64_t;
		static constexpr unsigned sig_bits = 52;
		static constexpr unsigned exp_bits = 11;
		static constexpr int bias = 1023;
	};

	template <typename F>
	static F fp_from_bits(const typename fp_traits<F>::bits_t bits) noexcept
	{
		F out;
		__builtin_memcpy(&out, &bits, sizeof(out));
		return out;
	}

	/// The canonical quiet NaN of an F, which both estimates return for
	/// every input they cannot answer for.
	template <typename F>
	static F fp_canonical_nan() noexcept
	{
		using T = fp_traits<F>;
		using B = typename T::bits_t;
		constexpr B exp_max = (B(1) << T::exp_bits) - 1;
		return fp_from_bits<F>((exp_max << T::sig_bits) | (B(1) << (T::sig_bits - 1)));
	}

	/// Normalise a subnormal significand in place, returning how far it had
	/// to shift. The leading one is shifted out, leaving the significand in
	/// the same form a normal value has.
	template <typename F, typename B>
	static int fp_normalize(B& sig) noexcept
	{
		using T = fp_traits<F>;
		constexpr B sig_mask = (B(1) << T::sig_bits) - 1;
		int shift = 0;
		while ((sig & (B(1) << (T::sig_bits - 1))) == 0) {
			sig <<= 1;
			shift++;
		}
		sig = (sig << 1) & sig_mask;
		return shift;
	}

	template <typename F>
	static F vfrsqrt7(const F value) noexcept
	{
		using T = fp_traits<F>;
		using B = typename T::bits_t;
		constexpr B sig_mask = (B(1) << T::sig_bits) - 1;
		constexpr B exp_max = (B(1) << T::exp_bits) - 1;

		B bits;
		__builtin_memcpy(&bits, &value, sizeof(bits));
		const B sign = bits >> (T::sig_bits + T::exp_bits);
		int exp = int((bits >> T::sig_bits) & exp_max);
		B sig = bits & sig_mask;

		if (exp == 0 && sig == 0)  // +-0 gives an infinity of the same sign
			return fp_from_bits<F>((sign << (T::sig_bits + T::exp_bits))
				| (exp_max << T::sig_bits));
		if (exp == int(exp_max) && sig == 0 && sign == 0)
			return F(0);           // +inf gives +0
		// Everything else negative, and every NaN, is invalid.
		if (sign != 0 || (exp == int(exp_max) && sig != 0))
			return fp_canonical_nan<F>();

		// A subnormal is normalised first, so that the table is always
		// indexed by a significand whose leading one is in a known place.
		// The exponent goes negative, which the halving below expects.
		if (exp == 0)
			exp = -fp_normalize<F, B>(sig);

		const unsigned index = unsigned(((exp & 1) << 6)
			| ((sig >> (T::sig_bits - 6)) & 0x3F));
		const B out_sig = B(VFRSQRT7_TABLE[index]) << (T::sig_bits - 7);
		// The exponent of 1/sqrt(x) is -exp/2, which in biased form is
		// (3*bias - 1 - exp) / 2. It can never leave the normal range.
		const B out_exp = B((3 * T::bias - 1 - exp) / 2);
		return fp_from_bits<F>((out_exp << T::sig_bits) | out_sig);
	}

	template <typename F>
	static F vfrec7(const F value, const unsigned frm) noexcept
	{
		using T = fp_traits<F>;
		using B = typename T::bits_t;
		constexpr B sig_mask = (B(1) << T::sig_bits) - 1;
		constexpr B exp_max = (B(1) << T::exp_bits) - 1;

		B bits;
		__builtin_memcpy(&bits, &value, sizeof(bits));
		const B sign = bits >> (T::sig_bits + T::exp_bits);
		const B sign_bit = sign << (T::sig_bits + T::exp_bits);
		int exp = int((bits >> T::sig_bits) & exp_max);
		B sig = bits & sig_mask;

		if (exp == int(exp_max)) {
			if (sig != 0)
				return fp_canonical_nan<F>();
			return fp_from_bits<F>(sign_bit);       // +-inf gives a signed zero
		}
		if (exp == 0 && sig == 0)                   // +-0 gives an infinity
			return fp_from_bits<F>(sign_bit | (exp_max << T::sig_bits));

		if (exp == 0) {
			const int shift = fp_normalize<F, B>(sig);
			exp = -shift;
			// Normalising by two or more places puts the reciprocal past
			// the largest finite value. Which of infinity and that value
			// is returned depends on the rounding mode and the sign.
			if (shift >= 2) {
				const bool to_max =
					frm == 1                            // RTZ
					|| (frm == 2 && sign == 0)          // RDN, positive
					|| (frm == 3 && sign != 0);         // RUP, negative
				if (to_max)
					return fp_from_bits<F>(sign_bit
						| ((exp_max - 1) << T::sig_bits) | sig_mask);
				return fp_from_bits<F>(sign_bit | (exp_max << T::sig_bits));
			}
		}

		const unsigned index = unsigned((sig >> (T::sig_bits - 7)) & 0x7F);
		B out_sig = B(VFREC7_TABLE[index]) << (T::sig_bits - 7);
		int out_exp = 2 * T::bias - 1 - exp;
		if (out_exp == 0 || out_exp == -1) {
			// The result is subnormal, so the implicit leading one has to
			// become explicit; an exponent of -1 needs one shift more.
			out_sig = (out_sig >> 1) | (B(1) << (T::sig_bits - 1));
			if (out_exp == -1)
				out_sig >>= 1;
			out_exp = 0;
		}
		return fp_from_bits<F>(sign_bit | (B(out_exp) << T::sig_bits) | out_sig);
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

		// A loop reissues the same vsetvli on every pass, so vtype is very
		// nearly always already what it is being set to -- and then the whole
		// of the work is clamping AVL. (An equal vtype also rules out vill,
		// which reads back with its top bit set.)
		if (LIKELY(rvv.vtype() == vi.VLI.zimm)) {
			const uint64_t vlmax = rvv.vlmax();
			rvv.set_vl(avl < vlmax ? avl : vlmax);
			if (!rd_is_x0) cpu.reg(vi.VLI.rd) = rvv.vl();
			return;
		}

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
	VSETVL_PRINTER("vsetvli", (1u << 0b00) | (1u << 0b01)));

	VECTOR_INSTR(VSETIVLI,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32v_instruction vi { instr };
		auto& rvv = cpu.registers().rvv();
		const bool rd_is_x0 = vi.IVLI.rd == 0;
		const uint64_t avl   = vi.IVLI.uimm;          // 5-bit unsigned AVL
		// vtype is ten bits here; the two above it are the marker that
		// tells vsetivli apart from the other two configuration forms.
		const uint32_t vtype = vi.IVLI.zimm & 0x3FF;

		// Already configured this way, as in a loop: see VSETVLI above.
		if (LIKELY(rvv.vtype() == vtype)) {
			const uint64_t vlmax = rvv.vlmax();
			rvv.set_vl(avl < vlmax ? avl : vlmax);
			if (!rd_is_x0) cpu.reg(vi.IVLI.rd) = rvv.vl();
			return;
		}

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
	VSETVL_PRINTER("vsetivli", 1u << 0b11));

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
	VSETVL_PRINTER("vsetvl", 1u << 0b10));

	namespace
	{
		// One element between guest memory and a vector register group. The
		// element type only has to be the right *width*: loads and stores
		// move bits, and the sign of what they move is the consumer's
		// business.
		template <bool IsStore, typename CPU_t>
		RISCV_ALWAYS_INLINE static void mem_element(CPU_t& cpu, const unsigned eew,
			const unsigned vreg, const uint64_t i, const typename CPU_t::address_t addr)
		{
			auto& rvv = cpu.registers().rvv();
			auto& memory = cpu.machine().memory;
			// Vector accesses have no alignment requirement, so every one of
			// these goes through memcpy rather than a typed load.
			const auto move = [&] (auto& slot) {
				if constexpr (IsStore)
					memory.memcpy(addr, &slot, sizeof(slot));
				else
					memory.memcpy_out(&slot, addr, sizeof(slot));
			};
			switch (eew) {
				case 1: move(element_at<uint8_t> (rvv, vreg, i)); return;
				case 2: move(element_at<uint16_t>(rvv, vreg, i)); return;
				case 4: move(element_at<uint32_t>(rvv, vreg, i)); return;
				default: move(element_at<uint64_t>(rvv, vreg, i)); return;
			}
		}

		/**
		 * The unmasked, single-field, unit-stride load and store, which is
		 * the shape a vectorising compiler emits for a flat loop -- and the
		 * one whose whole transfer is a single contiguous run of bytes at
		 * both ends: unit-stride walks guest memory element by element, and
		 * the lanes behind a register group are contiguous too. So it is one
		 * block copy of vl*EEW bytes, tail included; copying only that many
		 * is what leaves the tail undisturbed.
		 *
		 * The decoder has already read every static field this needs -- nf,
		 * mew, mop, lumop and vm are all fixed by the opcode it routed here,
		 * and EEW arrives as the template argument -- so what is left is the
		 * part that vtype decides. Returns false for the cases vtype makes
		 * awkward, which the general handler then decodes properly, and for
		 * the ones that have to trap.
		 */
		template <bool IsStore, unsigned EewLog2, typename CPU_t>
		RISCV_ALWAYS_INLINE static bool unit_stride_transfer(CPU_t& cpu, const rv32v_instruction vi)
		{
			auto& rvv = cpu.registers().rvv();
			if (UNLIKELY(rvv.vill()))
				return false;

			// EMUL = LMUL * EEW / SEW, as a count of registers. A group of
			// one needs no alignment, which is the case worth branching for:
			// it covers every EEW at LMUL <= 1.
			const int emul = rvv.lmul_shift() + int(EewLog2) - int(rvv.encoded_sew());
			const unsigned vdata = vi.VL.vd;
			unsigned dregs = 1;
			if (UNLIKELY(emul > 0)) {
				if (UNLIKELY(emul > 3))
					return false;
				dregs = 1u << emul;
				if (UNLIKELY((vdata & (dregs - 1)) != 0 || vdata + dregs > 32))
					return false;
			} else if (UNLIKELY(emul < -3)) {
				return false;
			}

			// vl is left alone when vtype changes under it, so a vl that no
			// longer fits its group goes the long way round.
			const uint64_t bytes = uint64_t(rvv.vl()) << EewLog2;
			if (UNLIKELY(bytes > uint64_t(dregs) * VectorLane::size()))
				return false;
			// An empty transfer touches no memory, and so cannot fault.
			if (UNLIKELY(bytes == 0))
				return true;

			auto& memory = cpu.machine().memory;
			const auto addr = (typename CPU_t::address_t)cpu.reg(vi.VL.rs1);
			void* lanes = &rvv.get(vdata);
			// One whole register is by far the common size, and naming it as
			// a constant is what lets the copy inline.
			if (LIKELY(bytes == VectorLane::size())) {
				if constexpr (IsStore)
					memory.memcpy(addr, lanes, VectorLane::size());
				else
					memory.memcpy_out(lanes, addr, VectorLane::size());
			} else {
				if constexpr (IsStore)
					memory.memcpy(addr, lanes, bytes);
				else
					memory.memcpy_out(lanes, addr, bytes);
			}
			return true;
		}

		// The i'th offset out of an index vector, zero-extended from the
		// index width the encoding names.
		template <typename RVV_t>
		static uint64_t index_offset(RVV_t& rvv, const unsigned eew,
			const unsigned vs2, const uint64_t i)
		{
			switch (eew) {
				case 1: return element_at<uint8_t> (rvv, vs2, i);
				case 2: return element_at<uint16_t>(rvv, vs2, i);
				case 4: return element_at<uint32_t>(rvv, vs2, i);
				default: return element_at<uint64_t>(rvv, vs2, i);
			}
		}

		/**
		 * Every vector load and store, in one place: the four addressing
		 * modes crossed with segments, plus the three transfers that ignore
		 * vtype entirely (whole-register, mask, and the whole-register
		 * store's byte-only form).
		 *
		 * The mnemonics differ far more than the work does. What actually
		 * varies is where each element's address comes from -- a running
		 * stride, a register stride, or an index vector -- and how many
		 * fields share that address, so the modes are decoded into a stride
		 * and a field count and then driven by one loop.
		 */
		template <bool IsStore, typename CPU_t>
		RISCV_NOINLINE static void vector_memory(CPU_t& cpu, const rv32v_instruction vi)
		{
			using address_t = typename CPU_t::address_t;
			auto& rvv = cpu.registers().rvv();
			auto& memory = cpu.machine().memory;

			const unsigned nf = vi.VL.nf + 1;   // fields per segment
			const unsigned vdata = vi.VL.vd;    // vs3 in a store
			const bool vm = vi.VL.vm;
			const address_t base = cpu.reg(vi.VL.rs1);

			// MEW would take the element past 64 bits, which no profile
			// defines and no toolchain emits.
			if (UNLIKELY(vi.VL.mew))
				cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION);
			const unsigned width_log2 = bits_lookup(vi.VL.width);
			if (UNLIKELY(width_log2 > 3))
				cpu.trigger_exception(ILLEGAL_OPCODE);
			// For an indexed access this is the width of the *index*; for
			// every other mode it is the width of the data.
			const unsigned encoded_eew = 1u << width_log2;

			if (vi.VL.mop == 0b00) {
				switch (vi.VL.lumop) {
				case 0b01000: {
					// vl<n>re<eew>.v / vs<n>r.v: a raw copy of whole
					// registers that reads neither vl nor vtype, with the
					// segment field carrying the register count instead.
					if (UNLIKELY(!vm || (nf != 1 && nf != 2 && nf != 4 && nf != 8)))
						cpu.trigger_exception(ILLEGAL_OPCODE);
					check_group(cpu, vdata, nf);
					for (unsigned r = 0; r < nf; r++) {
						const address_t at = base + r * VectorLane::size();
						if constexpr (IsStore)
							memory.memcpy(at, &rvv.get(vdata + r), VectorLane::size());
						else
							memory.memcpy_out(&rvv.get(vdata + r), at, VectorLane::size());
					}
					return; }
				case 0b01011: {
					// vlm.v / vsm.v: ceil(vl/8) bytes of mask, always EEW=8
					// with EMUL=1, and never masked.
					if (UNLIKELY(!vm || nf != 1 || vi.VL.width != 0))
						cpu.trigger_exception(ILLEGAL_OPCODE);
					require_valid_vtype(cpu);
					const uint64_t bytes = (rvv.vl() + 7) / 8;
					for (uint64_t b = 0; b < bytes; b++) {
						if constexpr (IsStore)
							memory.memcpy(base + b, &rvv.get(vdata).u8[b], 1);
						else
							memory.memcpy_out(&rvv.get(vdata).u8[b], base + b, 1);
					}
					return; }
				case 0b00000:
				case 0b10000:  // fault-only-first, handled below
					break;
				default:
					cpu.trigger_exception(ILLEGAL_OPCODE);
				}
			}
			require_valid_vtype(cpu);

			const bool indexed = (vi.VL.mop & 1) != 0; // 01 unordered, 11 ordered
			const bool strided = vi.VL.mop == 0b10;
			// Fault-only-first exists in the load direction only.
			const bool fault_first = !IsStore
				&& vi.VL.mop == 0b00 && vi.VL.lumop == 0b10000;
			if (UNLIKELY(IsStore && vi.VL.mop == 0b00 && vi.VL.lumop == 0b10000))
				cpu.trigger_exception(ILLEGAL_OPCODE);

			// An indexed access carries the data width in vtype and the
			// index width in the encoding; every other mode does the
			// reverse and has no index vector at all.
			const unsigned sew_bytes = rvv.sew() / 8;
			const unsigned data_eew = indexed ? sew_bytes : encoded_eew;
			const int demul = indexed
				? cpu.registers().rvv().lmul_shift() : emul_shift_for(cpu, data_eew);
			const unsigned dregs = group_size(demul);

			// NFIELDS * EMUL is what the segment forms actually consume.
			if (UNLIKELY(nf * dregs > 8 || vdata + nf * dregs > 32))
				cpu.trigger_exception(ILLEGAL_OPCODE);
			for (unsigned f = 0; f < nf; f++)
				check_group(cpu, vdata + f * dregs, dregs);

			if (indexed) {
				// The index group is sized by the index width against SEW.
				const int iemul = emul_shift_for(cpu, encoded_eew);
				check_group(cpu, vi.VLX.vs2, group_size(iemul));
			}

			const uint64_t vl = rvv.vl();
			// Unit-stride walks one whole segment per element.
			const address_t stride = strided
				? (address_t)cpu.reg(vi.VLS.rs2) : (address_t)(data_eew * nf);

			// The plain unmasked unit-stride transfer never arrives here:
			// unit_stride_transfer() above settles it as one block copy, and
			// hands back only the encodings that trap or that no longer fit
			// their register group.
			for (uint64_t i = 0; i < vl; i++) {
				if (!element_active(rvv, vm, i))
					continue;
				const address_t at = indexed
					? address_t(base + index_offset(rvv, encoded_eew, vi.VLX.vs2, i))
					: address_t(base + i * stride);
				if (fault_first && i != 0) {
					// Past the first element a fault is not reported: the
					// load stops there and shortens vl, which is what lets
					// a strlen-shaped loop read up to a page boundary
					// without knowing where the string ends.
					try {
						for (unsigned f = 0; f < nf; f++)
							mem_element<IsStore>(cpu, data_eew,
								vdata + f * dregs, i, at + f * data_eew);
					} catch (const MachineException&) {
						rvv.set_vl(i);
						return;
					}
					continue;
				}
				for (unsigned f = 0; f < nf; f++)
					mem_element<IsStore>(cpu, data_eew,
						vdata + f * dregs, i, at + f * data_eew);
			}
		}
	}

	VECTOR_INSTR(VLE32,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		vector_memory<false>(cpu, rv32v_instruction { instr });
	},
	VECTOR_MEM_PRINTER(false));

	VECTOR_INSTR(VSE32,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		vector_memory<true>(cpu, rv32v_instruction { instr });
	},
	VECTOR_MEM_PRINTER(true));

	/* The unit-stride loads and stores the decoder has already recognised:
	   one handler per direction and EEW, so that nothing static is left to
	   test and the block copy is all that remains. */
#define UNIT_STRIDE_INSTR(name, is_store, eew_log2) \
	VECTOR_INSTR(name, \
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR \
	{ \
		const rv32v_instruction vi { instr }; \
		if (LIKELY((unit_stride_transfer<is_store, eew_log2>(cpu, vi)))) \
			return; \
		vector_memory<is_store>(cpu, vi); \
	}, \
	VECTOR_MEM_PRINTER(is_store))

	UNIT_STRIDE_INSTR(VLE8_UNIT,  false, 0);
	UNIT_STRIDE_INSTR(VLE16_UNIT, false, 1);
	UNIT_STRIDE_INSTR(VLE32_UNIT, false, 2);
	UNIT_STRIDE_INSTR(VLE64_UNIT, false, 3);
	UNIT_STRIDE_INSTR(VSE8_UNIT,  true,  0);
	UNIT_STRIDE_INSTR(VSE16_UNIT, true,  1);
	UNIT_STRIDE_INSTR(VSE32_UNIT, true,  2);
	UNIT_STRIDE_INSTR(VSE64_UNIT, true,  3);
#undef UNIT_STRIDE_INSTR

	/* OPIVV: vd = vs2 OP vs1 */
	VECTOR_INSTR(VOPI_VV,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32v_instruction vi { instr };
		require_valid_vtype(cpu);
		auto& rvv = cpu.registers().rvv();
		const unsigned vd = vi.OPVV.vd, vs1 = vi.OPVV.vs1, vs2 = vi.OPVV.vs2;
		const bool vm = vi.OPVV.vm;
		{
			// Most code points here take three LMUL-sized groups, and each
			// has to be aligned to the group size. The ones that do not are
			// checked individually further down: the compares and
			// carry-outs write a single mask register rather than a group,
			// the narrowing shifts have a 2*SEW vs2, the widening
			// reductions have a single-register vd and vs1, and
			// vrgatherei16 sizes its index vector by EEW=16 instead of SEW.
			const unsigned f6 = vi.OPVV.funct6;
			const bool mask_dest = (f6 >= 0b011000 && f6 <= 0b011111)
				|| f6 == 0b010001 || f6 == 0b010011;
			if (f6 < 0b101100 && f6 != 0b001110) {
				if (!mask_dest)
					check_register_group(cpu, vd);
				check_register_group(cpu, vs1);
				check_register_group(cpu, vs2);
			}
		}

		switch (vi.OPVV.funct6) {
		case 0b000000: // VADD.VV
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t i) {
					return element_at<E>(rvv, vs2, i) + element_at<E>(rvv, vs1, i);
				});
			});
			return;
		case 0b000001: // VANDN.VV (Zvbb): vd = vs2 & ~vs1
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t i) {
					return E(element_at<E>(rvv, vs2, i) & ~element_at<E>(rvv, vs1, i));
				});
			});
			return;
		case 0b010100: // VROR.VV (Zvbb)
			vector_rotate(cpu, vi, true, 0, true);
			return;
		case 0b010101: // VROL.VV (Zvbb)
			vector_rotate(cpu, vi, true, 0, false);
			return;
		case 0b110101: // VWSLL.VV (Zvbb)
			vector_wide_shift(cpu, vi, true, 0);
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
		case 0b001110: // VRGATHEREI16.VV
			// The same gather, but the index vector is 16-bit whatever SEW
			// is, so one index register covers up to eight data registers.
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				const uint64_t vlmax = rvv.vlmax();
				vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t i) {
					const uint64_t idx = element_at<uint16_t>(rvv, vs1, i);
					return idx < vlmax ? element_at<E>(rvv, vs2, idx) : E(0);
				});
			});
			return;
		case 0b010000: // VADC.VVM: vd = vs2 + vs1 + v0 (masked encoding)
			if (vm)
				break; // vm=1 is reserved: v0 is the carry, not a mask
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				using U = std::make_unsigned_t<E>;
				vector_element_loop<E>(cpu, vd, true, [&] (uint64_t i) {
					return (E)((U)element_at<E>(rvv, vs2, i)
							+ (U)element_at<E>(rvv, vs1, i)
							+ rvv.get(0).mask(i));
				});
			});
			return;
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
			if (vm)
				break; // vm=1 is reserved, as for VADC
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				using U = std::make_unsigned_t<E>;
				vector_element_loop<E>(cpu, vd, true, [&] (uint64_t i) {
					return (E)((U)element_at<E>(rvv, vs2, i)
							- (U)element_at<E>(rvv, vs1, i)
							- rvv.get(0).mask(i));
				});
			});
			return;
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
		case 0b110000: // VWREDSUMU.VS
		case 0b110001: // VWREDSUM.VS
			// A reduction whose accumulator is 2*SEW: the scalar in vs1[0]
			// is already wide, and each SEW source element is extended
			// into it.
			widen_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				using U = std::make_unsigned_t<E>;
				using W = widen_t<E>;
				using WU = std::make_unsigned_t<W>;
				const bool sign = vi.OPVV.funct6 == 0b110001;
				const uint64_t vl = rvv.vl();
				if (vl == 0)
					return;
				W acc = element_at<W>(rvv, vs1, 0);
				for (uint64_t i = 0; i < vl; i++) {
					if (element_active(rvv, vm, i)) {
						const E x = element_at<E>(rvv, vs2, i);
						acc = (W)((WU)acc + (WU)(sign ? (W)x : (W)(WU)(U)x));
					}
				}
				element_at<W>(rvv, vd, 0) = acc;
			});
			return;
		} // switch
		// The fixed-point and narrowing shifts, which read their second
		// source out of vs1 just like everything above.
		if (integer_fixedpoint(cpu, vi, true, 0))
			return;
		cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION);
	},
	VECTOR_PRINTER("OPIVV", 1u << 0b000));

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
		{
			// As in the .vv handler: only the code points whose operands
			// really are LMUL-sized groups are checked here. See there for
			// which ones are not.
			const unsigned f6 = vi.OPVI.funct6;
			const bool mask_dest = (f6 >= 0b011000 && f6 <= 0b011111)
				|| f6 == 0b010001 || f6 == 0b010011;
			if (f6 < 0b100111) {
				if (!mask_dest)
					check_register_group(cpu, vd);
				check_register_group(cpu, vs2);
			}
		}

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
		case 0b000001: // VANDN.VX (Zvbb): vd = vs2 & ~x[rs1]
			if (!is_vx)
				break;      // there is no .vi form
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t i) {
					return E(element_at<E>(rvv, vs2, i) & ~(E)zimm);
				});
			});
			return;
		case 0b010100: // VROR.VX, or VROR.VI with the high immediate bit clear
		case 0b010101: // VROL.VX, or VROR.VI with it set
			// The .vi rotate needs six bits of immediate, and borrows the
			// low bit of funct6 for the sixth. That leaves the .vx forms
			// to tell rotate-left from rotate-right by the same bit.
			if (is_vx) {
				vector_rotate(cpu, vi, false, zimm, vi.OPVI.funct6 == 0b010100);
			} else {
				const uint64_t amount = ((vi.OPVI.funct6 & 1) << 5) | imm5;
				vector_rotate(cpu, vi, false, amount, true);
			}
			return;
		case 0b110101: // VWSLL.VX / VWSLL.VI (Zvbb)
			vector_wide_shift(cpu, vi, false, zimm);
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
		case 0b100111: { // VMV<nr>R.V in the .vi form; the .vx form is VSMUL
			if (is_vx)
				break;
			// Copies nregs whole registers, ignoring vl, vtype and mask.
			if (!vm)
				cpu.trigger_exception(ILLEGAL_OPCODE);
			const unsigned nregs = imm5 + 1;
			if (nregs != 1 && nregs != 2 && nregs != 4 && nregs != 8)
				cpu.trigger_exception(ILLEGAL_OPCODE);
			if ((vd % nregs) != 0 || (vs2 % nregs) != 0
				|| vd + nregs > 32 || vs2 + nregs > 32)
				cpu.trigger_exception(ILLEGAL_OPCODE);
			if (vd != vs2) {
				for (unsigned r = 0; r < nregs; r++)
					rvv.get(vd + r) = rvv.get(vs2 + r);
			}
			return; }
		} // switch
		// VSMUL, the scaling shifts and the narrowing shifts and clips.
		// The shift amount is unsigned in every one of them, and VNCVT.X.X.W
		// is just VNSRL.WX with a zero shift, so it needs no case of its own.
		if (integer_fixedpoint(cpu, vi, false, zimm))
			return;
		cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION);
		},
	VECTOR_PRINTER("OPIVI/OPIVX", (1u << 0b011) | (1u << 0b100)));

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
		// vs2 is only a register *group* in the arithmetic code points; the
		// scalar moves, the mask logic and vcompress all name single
		// registers, so the alignment check belongs with each of them
		// rather than out here.

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
			check_register_group(cpu, vs2);
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
		case 0b001000: // VAADDU: averaging add, unsigned
		case 0b001001: // VAADD:  ... signed
		case 0b001010: // VASUBU: averaging subtract, unsigned
		case 0b001011: // VASUB:  ... signed
			check_register_group(cpu, vd);
			check_register_group(cpu, vs2);
			if (!is_vx) check_register_group(cpu, vs1);
			integer_averaging(cpu, vi, !is_vx, (uint64_t)cpu.reg(vs1));
			return;
		case 0b010010: { // VZEXT/VSEXT.vfN, and the Zvbb bit operations
			if (is_vx) break;
			if (vs1 >= 0b00010 && vs1 <= 0b00111) {
				check_register_group(cpu, vd);
				int_extension(cpu, rvv, vd, vs2, vm, vs1);
				return;
			}
			if (zvbb_unary(cpu, vi, vs1))
				return;
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
		case 0b010100: { // the mask scans, plus VIOTA and VID
			if (is_vx) break;
			const uint64_t vl = rvv.vl();
			switch (vs1) {
			case 0b00001:   // VMSBF.M: set before the first set bit
			case 0b00010:   // VMSOF.M: set only the first set bit
			case 0b00011: { // VMSIF.M: set up to and including it
				bool seen_set = false;
				auto& src = rvv.get(vs2);
				auto& dest = rvv.get(vd);
				for (uint64_t i = 0; i < vl; i++) {
					if (!element_active(rvv, vm, i))
						continue;
					bool bit;
					if (seen_set) {
						bit = false;
					} else if (src.mask(i)) {
						// VMSBF stops short of the first set bit; the
						// other two include it.
						bit = vs1 != 0b00001;
						seen_set = true;
					} else {
						// Before it, only VMSOF is still writing zeroes.
						bit = vs1 != 0b00010;
					}
					dest.set_mask(i, bit);
				}
				return; }
			case 0b10000: { // VIOTA.M: prefix sum of the source mask
				check_register_group(cpu, vd);
				auto& src = rvv.get(vs2);
				uint64_t count = 0;
				int_sew_dispatch(cpu, [&] (auto tag) {
					using E = decltype(tag);
					for (uint64_t i = 0; i < vl; i++) {
						// Inactive elements are neither written nor
						// counted.
						if (!element_active(rvv, vm, i))
							continue;
						// Each element gets the count of set bits *below*
						// it, so the write comes before the increment.
						element_at<E>(rvv, vd, i) = (E)count;
						count += src.mask(i) ? 1 : 0;
					}
				});
				return; }
			case 0b10001: // VID.V: the element index itself
				if (vs2 != 0)
					break;
				check_register_group(cpu, vd);
				int_sew_dispatch(cpu, [&] (auto tag) {
					using E = decltype(tag);
					vector_element_loop<E>(cpu, vd, vm, [] (uint64_t i) {
						return (E)i;
					});
				});
				return;
			}
			break; }
		case 0b010111: { // VCOMPRESS.VM: pack the selected elements down
			if (is_vx || !vm)
				break;
			// The selector is vs1, not v0, and there is no masked form:
			// element j of the result is the j'th source element whose
			// selector bit is set, and the rest of vd is undisturbed.
			check_register_group(cpu, vd);
			check_register_group(cpu, vs2);
			const uint64_t vl = rvv.vl();
			auto& sel = rvv.get(vs1);
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				uint64_t j = 0;
				for (uint64_t i = 0; i < vl; i++) {
					if (sel.mask(i))
						element_at<E>(rvv, vd, j++) = element_at<E>(rvv, vs2, i);
				}
			});
			return; }
		case 0b011000: // VMANDN:  vd = vs2 & ~vs1
		case 0b011001: // VMAND:   vd = vs2 & vs1
		case 0b011010: // VMOR:    vd = vs2 | vs1
		case 0b011011: // VMXOR:   vd = vs2 ^ vs1
		case 0b011100: // VMORN:   vd = vs2 | ~vs1
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
					case 0b011100: r = a | ~c;  break;
					case 0b011101: r = ~(a & c); break;
					case 0b011110: r = ~(a | c); break;
					default:        r = ~(a ^ c); break;
				}
				rvv.get(vd).u8[b] = r;
			}
			return;
		case 0b100000: // VDIVU
		case 0b100001: // VDIV
		case 0b100010: // VREMU
		case 0b100011: // VREM
			check_register_group(cpu, vd);
			check_register_group(cpu, vs2);
			if (!is_vx) check_register_group(cpu, vs1);
			integer_divide(cpu, vi, !is_vx, (uint64_t)cpu.reg(vs1));
			return;
		case 0b100100: // VMULHU: high half, unsigned * unsigned
		case 0b100101: // VMUL:   the low SEW bits of the product
		case 0b100110: // VMULHSU: high half, signed vs2 * unsigned vs1
		case 0b100111: // VMULH:  high half, signed * signed
		case 0b101001: // VMADD:  vd = +(vs1 * vd) + vs2
		case 0b101011: // VNMSUB: vd = -(vs1 * vd) + vs2
		case 0b101101: // VMACC:  vd = +(vs1 * vs2) + vd
		case 0b101111: // VNMSAC: vd = -(vs1 * vs2) + vd
			check_register_group(cpu, vd);
			check_register_group(cpu, vs2);
			if (!is_vx) check_register_group(cpu, vs1);
			int_sew_dispatch(cpu, [&] (auto tag) {
				using E = decltype(tag);
				using U = std::make_unsigned_t<E>;
				const unsigned funct6 = vi.OPVV.funct6;
				vector_element_loop<E>(cpu, vd, vm, [&] (uint64_t i) {
					const E a = element_at<E>(rvv, vs2, i);
					const E b = is_vx ? (E)cpu.reg(vs1)
					                  : element_at<E>(rvv, vs1, i);
					const U c = element_at<E>(rvv, vd, i);
					switch (funct6) {
						case 0b100100: return mul_high<E>(a, b, false, false);
						case 0b100101: return (E)((U)a * (U)b);
						case 0b100110:
							// Only vs2 is signed, so the unsigned operand is
							// widened first and the product stays signed.
							return mul_high<E>(a, b, true, false);
						case 0b100111: return mul_high<E>(a, b, true, true);
						case 0b101001: return (E)((U)b * c + (U)a);
						case 0b101011: return (E)((U)a - (U)b * c);
						case 0b101101: return (E)((U)a * (U)b + c);
						default:       return (E)(c - (U)a * (U)b);
					}
				});
			});
			return;
		} // switch
		// The widening arithmetic, which occupies the whole top quarter of
		// the table.
		if (integer_widening(cpu, vi, !is_vx, (uint64_t)cpu.reg(vs1)))
			return;
		cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION);
	},
	VECTOR_PRINTER("OPMVV/OPMVX", (1u << 0b010) | (1u << 0b110)));

	namespace
	{
		/**
		 * The element-wise floating-point arithmetic of OPFVV: one result per
		 * element out of three LMUL-sized register groups, with no widening,
		 * no reduction and no mask destination. It is the body of every
		 * vectorised floating-point loop, and the decoder can pick it out on
		 * funct6 alone -- so it lives here, apart from the frame and the
		 * operand classification that the rest of the family needs.
		 *
		 * Returns false for a funct6 belonging to the rest, which the general
		 * handler then decodes from the top.
		 */
		template <typename CPU_t>
		RISCV_NOINLINE static bool opfvv_arith(CPU_t& cpu, const rv32v_instruction vi)
		{
			const unsigned funct6 = vi.OPVV.funct6;
			if (!RVV_IS_OPFVV_ARITH(funct6))
				return false;

			require_valid_vtype(cpu);
			auto& rvv = cpu.registers().rvv();
			const unsigned vd = vi.OPVV.vd, vs1 = vi.OPVV.vs1, vs2 = vi.OPVV.vs2;
			const bool vm = vi.OPVV.vm;
			check_register_group(cpu, vd);
			check_register_group(cpu, vs1);
			check_register_group(cpu, vs2);

			switch (funct6) {
			case 0b000000: // VFADD.VV
				fp_sew_dispatch_inline(cpu, [&] (auto tag) {
					using F = decltype(tag);
					vector_element_loop<F>(cpu, vd, vm, [&] (uint64_t i) {
						return element_at<F>(rvv, vs2, i) + element_at<F>(rvv, vs1, i);
					});
				});
				return true;
			case 0b000010: // VFSUB.VV
				fp_sew_dispatch_inline(cpu, [&] (auto tag) {
					using F = decltype(tag);
					vector_element_loop<F>(cpu, vd, vm, [&] (uint64_t i) {
						return element_at<F>(rvv, vs2, i) - element_at<F>(rvv, vs1, i);
					});
				});
				return true;
			case 0b000100: // VFMIN.VV (number-aware: NaN returns the other operand)
			case 0b000110: // VFMAX.VV
				fp_sew_dispatch_inline(cpu, [&] (auto tag) {
					using F = decltype(tag);
					const bool is_min = funct6 == 0b000100;
					vector_element_loop<F>(cpu, vd, vm, [&] (uint64_t i) {
						const F a = element_at<F>(rvv, vs2, i), b = element_at<F>(rvv, vs1, i);
						if (std::isnan(a)) return b;
						if (std::isnan(b)) return a;
						return is_min ? (a < b ? a : b) : (a > b ? a : b);
					});
				});
				return true;
			case 0b001000: // VFSGNJ.VV
			case 0b001001: // VFSGNJN.VV
			case 0b001010: // VFSGNJX.VV
				fp_sew_dispatch_inline(cpu, [&] (auto tag) {
					using F = decltype(tag);
					using B = std::conditional_t<sizeof(F) == 4, uint32_t, uint64_t>;
					constexpr B sign_bit = B(1) << (8 * sizeof(B) - 1);
					// Sign injection moves bits, not values, so the elements
					// are read and written as raw patterns of the same width.
					vector_element_loop<B>(cpu, vd, vm, [&] (uint64_t i) {
						const B a = element_at<B>(rvv, vs2, i);
						const B b = element_at<B>(rvv, vs1, i);
						if (funct6 == 0b001000) return B((a & ~sign_bit) | (b & sign_bit));
						if (funct6 == 0b001001) return B((a & ~sign_bit) | (~b & sign_bit));
						return B(a ^ (b & sign_bit));
					});
				});
				return true;
			case 0b100000: // VFDIV.VV
				fp_sew_dispatch_inline(cpu, [&] (auto tag) {
					using F = decltype(tag);
					vector_element_loop<F>(cpu, vd, vm, [&] (uint64_t i) {
						return element_at<F>(rvv, vs2, i) / element_at<F>(rvv, vs1, i);
					});
				});
				return true;
			case 0b100100: // VFMUL.VV
				fp_sew_dispatch_inline(cpu, [&] (auto tag) {
					using F = decltype(tag);
					vector_element_loop<F>(cpu, vd, vm, [&] (uint64_t i) {
						return element_at<F>(rvv, vs2, i) * element_at<F>(rvv, vs1, i);
					});
				});
				return true;
			default: // The eight fused multiply-adds, see below.
				fp_sew_dispatch_inline(cpu, [&] (auto tag) {
					using F = decltype(tag);
					vector_element_loop<F>(cpu, vd, vm, [&] (uint64_t i) {
						const F a = element_at<F>(rvv, vs1, i);
						const F b = element_at<F>(rvv, vs2, i);
						const F c = element_at<F>(rvv, vd, i);
						// These are fused: the product is not rounded before
						// the addition, so they go through std::fma just like
						// the scalar FMADD family does. The four `n` forms
						// negate the whole expression, which is the same as
						// negating one factor and the addend.
						switch (funct6) {
							case 0b101000: return  std::fma(a, c, b);  // VFMADD:  +(vs1*vd) + vs2
							case 0b101001: return -std::fma(a, c, b);  // VFNMADD: -(vs1*vd) - vs2
							case 0b101010: return  std::fma(a, c, -b); // VFMSUB:  +(vs1*vd) - vs2
							case 0b101011: return  std::fma(-a, c, b); // VFNMSUB: -(vs1*vd) + vs2
							case 0b101100: return  std::fma(a, b, c);  // VFMACC:  +(vs1*vs2) + vd
							case 0b101101: return -std::fma(a, b, c);  // VFNMACC: -(vs1*vs2) - vd
							case 0b101110: return  std::fma(a, b, -c); // VFMSAC:  +(vs1*vs2) - vd
							default:       return  std::fma(-a, b, c); // VFNMSAC: -(vs1*vs2) + vd
						}
					});
				});
				return true;
			}
		}
	}

	/* OPFVV: vd = vs2 OP vs1 */
	VECTOR_INSTR(VOPF_VV,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32v_instruction vi { instr };
		// The element-wise arithmetic has a handler of its own that the
		// decoder reaches directly; this is the path for an encoding it did
		// not recognise, and for the rest of the family.
		if (opfvv_arith(cpu, vi))
			return;

		require_valid_vtype(cpu);
		auto& rvv = cpu.registers().rvv();
		const unsigned vd = vi.OPVV.vd, vs1 = vi.OPVV.vs1, vs2 = vi.OPVV.vs2;
		const bool vm = vi.OPVV.vm;
		{
			// Of what is left, only the non-widening arithmetic has three
			// LMUL-sized groups. The reductions have a single-register vd and
			// vs1, the compares write a mask register, the unary group covers
			// the scalar moves and the widening conversions, and everything
			// from funct6 110000 up is widening.
			const unsigned f6 = vi.OPVV.funct6;
			const bool reduce = f6 < 0b001000 && (f6 & 1);
			const bool mask_dest = f6 >= 0b011000 && f6 <= 0b011111;
			const bool unary = f6 == 0b010000 || f6 == 0b010010 || f6 == 0b010011;
			if (f6 < 0b110000 && !reduce && !mask_dest && !unary) {
				check_register_group(cpu, vd);
				check_register_group(cpu, vs1);
				check_register_group(cpu, vs2);
			}
		}

		switch (vi.OPVV.funct6) {
		case 0b000001: // VFREDUSUM.VS
		case 0b000011: // VFREDOSUM.VS (ordered sum)
			fp_sew_dispatch(cpu, [&] (auto tag) {
				using F = decltype(tag);
				reduction_loop<F>(cpu, vd, vs1, vs2, vm, [] (F acc, F x) -> F {
					return acc + x;
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
		case 0b010011: // VFSQRT.V and the two estimates, plus VFCLASS.V
			if (vs1 == 0b00000) {
				fp_sew_dispatch(cpu, [&] (auto tag) {
					using F = decltype(tag);
					vector_element_loop<F>(cpu, vd, vm, [&] (uint64_t i) {
						return std::sqrt(element_at<F>(rvv, vs2, i));
					});
				});
				return;
			} else if (vs1 == 0b00100) { // VFRSQRT7.V
				fp_sew_dispatch(cpu, [&] (auto tag) {
					using F = decltype(tag);
					vector_element_loop<F>(cpu, vd, vm, [&] (uint64_t i) {
						return vfrsqrt7(element_at<F>(rvv, vs2, i));
					});
				});
				return;
			} else if (vs1 == 0b00101) { // VFREC7.V
				const unsigned frm = cpu.registers().fcsr().frm;
				fp_sew_dispatch(cpu, [&] (auto tag) {
					using F = decltype(tag);
					vector_element_loop<F>(cpu, vd, vm, [&] (uint64_t i) {
						return vfrec7(element_at<F>(rvv, vs2, i), frm);
					});
				});
				return;
			} else if (vs1 == 0b10000) { // VFCLASS.V
				// The classification is a ten-bit value written to an
				// SEW-wide element, exactly like the scalar fclass writes
				// an integer register -- not a mask.
				fp_sew_dispatch(cpu, [&] (auto tag) {
					using F = decltype(tag);
					using B = std::conditional_t<sizeof(F) == 4, uint32_t, uint64_t>;
					vector_element_loop<B>(cpu, vd, vm, [&] (uint64_t i) {
						return (B)fp_class_mask(element_at<F>(rvv, vs2, i));
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
		} // switch
		// The widening arithmetic and the two widening reductions.
		if (float_widening(cpu, vi, true, 0.0f))
			return;
		cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION);
	},
	VECTOR_PRINTER("OPFVV", 1u << 0b001));

	/* OPFVV, element-wise arithmetic: the same instruction family, entered
	   where the decoder has already read funct6 and knows this is all there
	   is to do. */
	VECTOR_INSTR(VOPF_VV_ARITH,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		opfvv_arith(cpu, rv32v_instruction { instr });
	},
	VECTOR_PRINTER("OPFVV", 1u << 0b001));

	/* OPFVF: vd = vs2 OP f[rs1] */
	VECTOR_INSTR(VOPF_VF,
	[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR
	{
		const rv32v_instruction vi { instr };
		require_valid_vtype(cpu);
		auto& rvv = cpu.registers().rvv();
		const unsigned vd = vi.OPVV.vd, vs1 = vi.OPVV.vs1, vs2 = vi.OPVV.vs2;
		const bool vm = vi.OPVV.vm;
		{
			// The second source is an f register here, so only vd and vs2
			// can be groups -- and not for the compares, which write a mask,
			// nor for vfmv.s.f, whose destination is one element.
			const unsigned f6 = vi.OPVV.funct6;
			const bool mask_dest = f6 >= 0b011000 && f6 <= 0b011111;
			if (f6 < 0b110000 && !mask_dest && f6 != 0b010000) {
				check_register_group(cpu, vd);
				check_register_group(cpu, vs2);
			}
		}

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
				// As in the .vv form, these move bit patterns; only the
				// scalar's sign bit is actually used.
				B b;
				__builtin_memcpy(&b, &sf, sizeof(b));
				vector_element_loop<B>(cpu, vd, vm, [&] (uint64_t i) {
					const B a = element_at<B>(rvv, vs2, i);
					if (funct6 == 0b001000) return B((a & ~sign_bit) | (b & sign_bit));
					if (funct6 == 0b001001) return B((a & ~sign_bit) | (~b & sign_bit));
					return B(a ^ (b & sign_bit));
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
					// Fused, as in the .vv forms above.
					switch (funct6) {
						case 0b101000: return  std::fma(s, c, b);
						case 0b101001: return -std::fma(s, c, b);
						case 0b101010: return  std::fma(s, c, -b);
						case 0b101011: return  std::fma(-s, c, b);
						case 0b101100: return  std::fma(s, b, c);
						case 0b101101: return -std::fma(s, b, c);
						case 0b101110: return  std::fma(s, b, -c);
						default:       return  std::fma(-s, b, c);
					}
				});
			});
			return;
		} // switch
		// The widening arithmetic. Its sources are single precision, so it
		// takes the f32 scalar whatever SEW says.
		if (float_widening(cpu, vi, false, scalar_f))
			return;
		cpu.trigger_exception(UNIMPLEMENTED_INSTRUCTION);
	},
	VECTOR_PRINTER("OPFVF", 1u << 0b101));
} // riscv
