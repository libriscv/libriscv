#pragma once
#include "rv_printer.hpp"
#include "rvv.hpp"

/**
 * Instruction printers for the vector extension, RVV 1.0.
 *
 * OP-V is a 64-entry funct6 table crossed with funct3, and funct3 is what picks
 * where the second source comes from: another vector register (.vv), an integer
 * register (.vx), a 5-bit immediate (.vi) or a float register (.vf). Most code
 * points exist in only some of those, so every table entry carries both the set
 * it is defined for and the shape its operands print in. Anything outside comes
 * out as `.insn`, which is what binutils prints for an encoding it does not
 * recognise.
 *
 * The code points that are not funct6-uniform are matched ahead of the tables:
 * the ones that reuse the source field as an opcode extension (vzext, the mask
 * instructions, the conversions), and the ones that require some other field to
 * be zero because they have no operand for it (vmv.v.v, vid.v and friends).
 */
namespace riscv
{
	struct RVVDISASM
	{
		using instr_t = rv32i_instruction;

		static int bad(char* b, size_t n, instr_t i) noexcept
		{
			return RVPRINT::illegal(b, n, i.whole, 4);
		}

		/// objdump spells a masked operation by appending `,v0.t`.
		static const char* vmask(uint32_t vm) noexcept
		{
			return vm ? "" : ",v0.t";
		}

		/// The funct3 encodings a code point can exist in. A float code point
		/// takes its scalar from an f-register rather than an x-register, but
		/// no code point has both forms, so the two share a flag.
		enum : uint8_t {
			AS_VV = 1,        ///< second source is a vector register
			AS_VX = 2,        ///< ... an integer register
			AS_VI = 4,        ///< ... a 5-bit immediate
			AS_VF = AS_VX,    ///< ... a float register
		};

		/// How a code point's operands print.
		enum : uint8_t {
			SH_ARITH,   ///< vd,vs2,src      -- the .vi immediate is signed
			SH_SHIFT,   ///< vd,vs2,src      -- the .vi immediate is unsigned
			SH_WIDE,    ///< as SH_SHIFT, but the suffix is .wv/.wx/.wi/.wf
			SH_REDUCE,  ///< vd,vs2,vs1      -- .vs suffix, no scalar form
			SH_MADD,    ///< vd,src,vs2      -- multiply-add, which also reads vd
			SH_CARRY,   ///< vd,vs2,src,v0   -- carry-in: masked encoding only
			SH_MCARRY,  ///< carry-out: the `,v0` operand is the masked encoding
			SH_WHOLE,   ///< vd,vs2,vs1, never masked; the name carries its suffix
		};

		struct Entry {
			const char* name;  ///< nullptr when the code point is reserved
			uint8_t     forms; ///< the AS_* set this code point is defined for
			uint8_t     shape; ///< SH_*
		};

		/// The mnemonic suffix for one form, `.w*` for the instructions whose
		/// vs2 is twice as wide as their result (or their other source).
		static const char* suffix(uint8_t form, bool wide, bool is_float) noexcept
		{
			if (form == AS_VV) return wide ? "wv" : "vv";
			if (form == AS_VI) return wide ? "wi" : "vi";
			if (is_float)      return wide ? "wf" : "vf";
			return wide ? "wx" : "vx";
		}

		/// The second source operand. `buf` backs the immediate forms, which
		/// are signed for arithmetic and compares, unsigned everywhere else.
		static const char* source(char* buf, size_t len, uint8_t form,
			uint32_t field, bool is_signed, bool is_float) noexcept
		{
			if (form == AS_VV) return RVPRINT::vreg(field);
			if (form == AS_VX) return is_float ? RVPRINT::freg(field) : RVPRINT::reg(field);
			if (is_signed)
				snprintf(buf, len, "%d", int32_t(field ^ 0x10) - 0x10);
			else
				snprintf(buf, len, "%u", field);
			return buf;
		}

		/// Print one table entry, once the specials have been ruled out.
		static int shaped(char* b, size_t n, instr_t i, const Entry& op,
			uint8_t form, bool is_float) noexcept
		{
			const rv32v_instruction v { i };
			const uint32_t vd = v.OPVV.vd, vs2 = v.OPVV.vs2, vm = v.OPVV.vm;
			const uint32_t field = v.OPVV.vs1; // vs1, rs1 and the immediate share it

			char imm[16];
			const bool wide = op.shape == SH_WIDE;
			const char* sfx = suffix(form, wide, is_float);
			const char* src = source(imm, sizeof(imm), form, field,
				op.shape == SH_ARITH || op.shape == SH_CARRY || op.shape == SH_MCARRY,
				is_float);
			const char* vrd = RVPRINT::vreg(vd);

			switch (op.shape) {
			case SH_ARITH:
			case SH_SHIFT:
			case SH_WIDE:
				return snprintf(b, n, "%s.%s\t%s,%s,%s%s",
					op.name, sfx, vrd, RVPRINT::vreg(vs2), src, vmask(vm));
			case SH_REDUCE:
				return snprintf(b, n, "%s.vs\t%s,%s,%s%s",
					op.name, vrd, RVPRINT::vreg(vs2), src, vmask(vm));
			case SH_MADD:
				// The multiply-add family names its scalar source first, since
				// the wide operand is the accumulator in vd.
				return snprintf(b, n, "%s.%s\t%s,%s,%s%s",
					op.name, sfx, vrd, src, RVPRINT::vreg(vs2), vmask(vm));
			case SH_CARRY:
				// The carry comes from v0, so there is no room for a mask and
				// only the vm=0 encoding exists.
				if (vm)
					return bad(b, n, i);
				return snprintf(b, n, "%s.%sm\t%s,%s,%s,v0",
					op.name, sfx, vrd, RVPRINT::vreg(vs2), src);
			case SH_MCARRY:
				// Carry-out: vm selects whether there is a carry *in* as well.
				if (vm)
					return snprintf(b, n, "%s.%s\t%s,%s,%s",
						op.name, sfx, vrd, RVPRINT::vreg(vs2), src);
				return snprintf(b, n, "%s.%sm\t%s,%s,%s,v0",
					op.name, sfx, vrd, RVPRINT::vreg(vs2), src);
			default: // SH_WHOLE
				// Mask-register logic and vcompress read whole registers, so
				// they take neither a mask nor an element width.
				if (!vm)
					return bad(b, n, i);
				return snprintf(b, n, "%s\t%s,%s,%s",
					op.name, vrd, RVPRINT::vreg(vs2), src);
			}
		}

		/// `vzext.vf2 v8,v12`: an operation whose only source is vs2, and whose
		/// name already carries the suffix that would otherwise say so.
		static int unary(char* b, size_t n, const char* m, instr_t i) noexcept
		{
			const rv32v_instruction v { i };
			return snprintf(b, n, "%s\t%s,%s%s", m, RVPRINT::vreg(v.OPVV.vd),
				RVPRINT::vreg(v.OPVV.vs2), vmask(v.OPVV.vm));
		}

		// ---- OPIVV / OPIVX / OPIVI -----------------------------------------

		static int op_integer(char* b, size_t n, instr_t i, uint8_t form) noexcept
		{
			static const Entry table[64] = {
			/* 00 */ { "vadd",       AS_VV|AS_VX|AS_VI, SH_ARITH },
			/* 01 */ { "vandn",     AS_VV|AS_VX,       SH_ARITH }, // Zvbb
			/* 02 */ { "vsub",       AS_VV|AS_VX,       SH_ARITH },
			/* 03 */ { "vrsub",            AS_VX|AS_VI, SH_ARITH },
			/* 04 */ { "vminu",      AS_VV|AS_VX,       SH_ARITH },
			/* 05 */ { "vmin",       AS_VV|AS_VX,       SH_ARITH },
			/* 06 */ { "vmaxu",      AS_VV|AS_VX,       SH_ARITH },
			/* 07 */ { "vmax",       AS_VV|AS_VX,       SH_ARITH },
			/* 08 */ { nullptr,      0,                 SH_ARITH },
			/* 09 */ { "vand",       AS_VV|AS_VX|AS_VI, SH_ARITH },
			/* 10 */ { "vor",        AS_VV|AS_VX|AS_VI, SH_ARITH },
			/* 11 */ { "vxor",       AS_VV|AS_VX|AS_VI, SH_ARITH },
			/* 12 */ { "vrgather",   AS_VV|AS_VX|AS_VI, SH_SHIFT },
			/* 13 */ { nullptr,      0,                 SH_ARITH },
			/* 14 */ { "vslideup",         AS_VX|AS_VI, SH_SHIFT }, // .vv is VRGATHEREI16
			/* 15 */ { "vslidedown",       AS_VX|AS_VI, SH_SHIFT },
			/* 16 */ { "vadc",       AS_VV|AS_VX|AS_VI, SH_CARRY },
			/* 17 */ { "vmadc",      AS_VV|AS_VX|AS_VI, SH_MCARRY },
			/* 18 */ { "vsbc",       AS_VV|AS_VX,       SH_CARRY },
			/* 19 */ { "vmsbc",      AS_VV|AS_VX,       SH_MCARRY },
			/* 20 */ { "vror",       AS_VV|AS_VX,       SH_SHIFT }, // Zvbb; .vi below
			/* 21 */ { "vrol",       AS_VV|AS_VX,       SH_SHIFT }, // Zvbb
			/* 22 */ { nullptr,      0,                 SH_ARITH },
			/* 23 */ { nullptr,      0,                 SH_ARITH }, // VMERGE / VMV.V.*
			/* 24 */ { "vmseq",      AS_VV|AS_VX|AS_VI, SH_ARITH },
			/* 25 */ { "vmsne",      AS_VV|AS_VX|AS_VI, SH_ARITH },
			/* 26 */ { "vmsltu",     AS_VV|AS_VX,       SH_ARITH },
			/* 27 */ { "vmslt",      AS_VV|AS_VX,       SH_ARITH },
			/* 28 */ { "vmsleu",     AS_VV|AS_VX|AS_VI, SH_ARITH },
			/* 29 */ { "vmsle",      AS_VV|AS_VX|AS_VI, SH_ARITH },
			/* 30 */ { "vmsgtu",           AS_VX|AS_VI, SH_ARITH },
			/* 31 */ { "vmsgt",            AS_VX|AS_VI, SH_ARITH },
			/* 32 */ { "vsaddu",     AS_VV|AS_VX|AS_VI, SH_ARITH },
			/* 33 */ { "vsadd",      AS_VV|AS_VX|AS_VI, SH_ARITH },
			/* 34 */ { "vssubu",     AS_VV|AS_VX,       SH_ARITH },
			/* 35 */ { "vssub",      AS_VV|AS_VX,       SH_ARITH },
			/* 36 */ { nullptr,      0,                 SH_ARITH },
			/* 37 */ { "vsll",       AS_VV|AS_VX|AS_VI, SH_SHIFT },
			/* 38 */ { nullptr,      0,                 SH_ARITH },
			/* 39 */ { "vsmul",      AS_VV|AS_VX,       SH_ARITH }, // .vi is VMV<nr>R.V
			/* 40 */ { "vsrl",       AS_VV|AS_VX|AS_VI, SH_SHIFT },
			/* 41 */ { "vsra",       AS_VV|AS_VX|AS_VI, SH_SHIFT },
			/* 42 */ { "vssrl",      AS_VV|AS_VX|AS_VI, SH_SHIFT },
			/* 43 */ { "vssra",      AS_VV|AS_VX|AS_VI, SH_SHIFT },
			/* 44 */ { "vnsrl",      AS_VV|AS_VX|AS_VI, SH_WIDE },
			/* 45 */ { "vnsra",      AS_VV|AS_VX|AS_VI, SH_WIDE },
			/* 46 */ { "vnclipu",    AS_VV|AS_VX|AS_VI, SH_WIDE },
			/* 47 */ { "vnclip",     AS_VV|AS_VX|AS_VI, SH_WIDE },
			/* 48 */ { "vwredsumu",  AS_VV,             SH_REDUCE },
			/* 49 */ { "vwredsum",   AS_VV,             SH_REDUCE },
			/* 50 */ { nullptr,      0,                 SH_ARITH },
			/* 51 */ { nullptr,      0,                 SH_ARITH },
			/* 52 */ { nullptr,      0,                 SH_ARITH },
			/* 53 */ { "vwsll",      AS_VV|AS_VX|AS_VI, SH_SHIFT }, // Zvbb
			};

			const rv32v_instruction v { i };
			const uint32_t f6 = v.OPVV.funct6, vd = v.OPVV.vd;
			const uint32_t vs2 = v.OPVV.vs2, field = v.OPVV.vs1, vm = v.OPVV.vm;

			// VMERGE and VMV.V.* share a code point: the move is the unmasked
			// form, and has no vector source for vs2 to name.
			if (f6 == 0b010111) {
				char imm[16];
				const char* src = source(imm, sizeof(imm), form, field, true, false);
				if (!vm)
					return snprintf(b, n, "vmerge.%sm\t%s,%s,%s,v0",
						suffix(form, false, false), RVPRINT::vreg(vd),
						RVPRINT::vreg(vs2), src);
				if (vs2 != 0)
					return bad(b, n, i);
				return snprintf(b, n, "vmv.v.%c\t%s,%s",
					form == AS_VV ? 'v' : form == AS_VX ? 'x' : 'i',
					RVPRINT::vreg(vd), src);
			}
			// The whole-register move borrows VSMUL's code point in the .vi
			// form; the immediate is the register count, less one.
			if (f6 == 0b100111 && form == AS_VI) {
				const unsigned nregs = field + 1;
				if (!vm || (nregs != 1 && nregs != 2 && nregs != 4 && nregs != 8))
					return bad(b, n, i);
				return snprintf(b, n, "vmv%ur.v\t%s,%s", nregs,
					RVPRINT::vreg(vd), RVPRINT::vreg(vs2));
			}
			// VROR.VI needs six bits of rotate amount and borrows the low
			// bit of funct6 for the sixth, so its two halves are a pair of
			// code points rather than one. Rotate-left has no .vi form: it
			// is the same instruction with the amount negated.
			if ((f6 & 0b111110) == 0b010100 && form == AS_VI) {
				return snprintf(b, n, "vror.vi\t%s,%s,%u%s",
					RVPRINT::vreg(vd), RVPRINT::vreg(vs2),
					((f6 & 1) << 5) | field, vmask(vm));
			}
			// VRGATHEREI16 shares VSLIDEUP's code point, in the .vv form only.
			if (f6 == 0b001110 && form == AS_VV) {
				static const Entry gather = { "vrgatherei16", AS_VV, SH_ARITH };
				return shaped(b, n, i, gather, form, false);
			}

			const Entry& op = table[f6];
			if (op.name == nullptr || !(op.forms & form))
				return bad(b, n, i);
			return shaped(b, n, i, op, form, false);
		}

		// ---- OPMVV / OPMVX -------------------------------------------------

		static int op_mask(char* b, size_t n, instr_t i, uint8_t form) noexcept
		{
			static const Entry table[64] = {
			/* 00 */ { "vredsum",    AS_VV,       SH_REDUCE },
			/* 01 */ { "vredand",    AS_VV,       SH_REDUCE },
			/* 02 */ { "vredor",     AS_VV,       SH_REDUCE },
			/* 03 */ { "vredxor",    AS_VV,       SH_REDUCE },
			/* 04 */ { "vredminu",   AS_VV,       SH_REDUCE },
			/* 05 */ { "vredmin",    AS_VV,       SH_REDUCE },
			/* 06 */ { "vredmaxu",   AS_VV,       SH_REDUCE },
			/* 07 */ { "vredmax",    AS_VV,       SH_REDUCE },
			/* 08 */ { "vaaddu",     AS_VV|AS_VX, SH_ARITH },
			/* 09 */ { "vaadd",      AS_VV|AS_VX, SH_ARITH },
			/* 10 */ { "vasubu",     AS_VV|AS_VX, SH_ARITH },
			/* 11 */ { "vasub",      AS_VV|AS_VX, SH_ARITH },
			/* 12 */ { nullptr,      0,           SH_ARITH },
			/* 13 */ { nullptr,      0,           SH_ARITH },
			/* 14 */ { "vslide1up",        AS_VX, SH_ARITH },
			/* 15 */ { "vslide1down",      AS_VX, SH_ARITH },
			/* 16 */ { nullptr,      0,           SH_ARITH }, // VMV.X.S / VMV.S.X
			/* 17 */ { nullptr,      0,           SH_ARITH },
			/* 18 */ { nullptr,      0,           SH_ARITH }, // VZEXT / VSEXT
			/* 19 */ { nullptr,      0,           SH_ARITH },
			/* 20 */ { nullptr,      0,           SH_ARITH }, // the mask scans
			/* 21 */ { nullptr,      0,           SH_ARITH },
			/* 22 */ { nullptr,      0,           SH_ARITH },
			/* 23 */ { "vcompress.vm", AS_VV,     SH_WHOLE },
			/* 24 */ { "vmandn.mm",  AS_VV,       SH_WHOLE },
			/* 25 */ { "vmand.mm",   AS_VV,       SH_WHOLE },
			/* 26 */ { "vmor.mm",    AS_VV,       SH_WHOLE },
			/* 27 */ { "vmxor.mm",   AS_VV,       SH_WHOLE },
			/* 28 */ { "vmorn.mm",   AS_VV,       SH_WHOLE },
			/* 29 */ { "vmnand.mm",  AS_VV,       SH_WHOLE },
			/* 30 */ { "vmnor.mm",   AS_VV,       SH_WHOLE },
			/* 31 */ { "vmxnor.mm",  AS_VV,       SH_WHOLE },
			/* 32 */ { "vdivu",      AS_VV|AS_VX, SH_ARITH },
			/* 33 */ { "vdiv",       AS_VV|AS_VX, SH_ARITH },
			/* 34 */ { "vremu",      AS_VV|AS_VX, SH_ARITH },
			/* 35 */ { "vrem",       AS_VV|AS_VX, SH_ARITH },
			/* 36 */ { "vmulhu",     AS_VV|AS_VX, SH_ARITH },
			/* 37 */ { "vmul",       AS_VV|AS_VX, SH_ARITH },
			/* 38 */ { "vmulhsu",    AS_VV|AS_VX, SH_ARITH },
			/* 39 */ { "vmulh",      AS_VV|AS_VX, SH_ARITH },
			/* 40 */ { nullptr,      0,           SH_ARITH },
			/* 41 */ { "vmadd",      AS_VV|AS_VX, SH_MADD },
			/* 42 */ { nullptr,      0,           SH_ARITH },
			/* 43 */ { "vnmsub",     AS_VV|AS_VX, SH_MADD },
			/* 44 */ { nullptr,      0,           SH_ARITH },
			/* 45 */ { "vmacc",      AS_VV|AS_VX, SH_MADD },
			/* 46 */ { nullptr,      0,           SH_ARITH },
			/* 47 */ { "vnmsac",     AS_VV|AS_VX, SH_MADD },
			/* 48 */ { "vwaddu",     AS_VV|AS_VX, SH_ARITH },
			/* 49 */ { "vwadd",      AS_VV|AS_VX, SH_ARITH },
			/* 50 */ { "vwsubu",     AS_VV|AS_VX, SH_ARITH },
			/* 51 */ { "vwsub",      AS_VV|AS_VX, SH_ARITH },
			/* 52 */ { "vwaddu",     AS_VV|AS_VX, SH_WIDE },
			/* 53 */ { "vwadd",      AS_VV|AS_VX, SH_WIDE },
			/* 54 */ { "vwsubu",     AS_VV|AS_VX, SH_WIDE },
			/* 55 */ { "vwsub",      AS_VV|AS_VX, SH_WIDE },
			/* 56 */ { "vwmulu",     AS_VV|AS_VX, SH_ARITH },
			/* 57 */ { nullptr,      0,           SH_ARITH },
			/* 58 */ { "vwmulsu",    AS_VV|AS_VX, SH_ARITH },
			/* 59 */ { "vwmul",      AS_VV|AS_VX, SH_ARITH },
			/* 60 */ { "vwmaccu",    AS_VV|AS_VX, SH_MADD },
			/* 61 */ { "vwmacc",     AS_VV|AS_VX, SH_MADD },
			/* 62 */ { "vwmaccus",         AS_VX, SH_MADD },
			/* 63 */ { "vwmaccsu",   AS_VV|AS_VX, SH_MADD },
			};

			const rv32v_instruction v { i };
			const uint32_t f6 = v.OPVV.funct6, vd = v.OPVV.vd;
			const uint32_t vs2 = v.OPVV.vs2, field = v.OPVV.vs1, vm = v.OPVV.vm;

			switch (f6) {
			case 0b010000:
				// Scalar moves and the two mask queries share a code point,
				// selected by the source field. The moves are never masked.
				if (form == AS_VX) {
					if (!vm || vs2 != 0)
						return bad(b, n, i);
					return snprintf(b, n, "vmv.s.x\t%s,%s",
						RVPRINT::vreg(vd), RVPRINT::reg(field));
				}
				if (field == 0b00000) {
					if (!vm)
						return bad(b, n, i);
					return snprintf(b, n, "vmv.x.s\t%s,%s",
						RVPRINT::reg(vd), RVPRINT::vreg(vs2));
				}
				if (field == 0b10000 || field == 0b10001)
					return snprintf(b, n, "%s\t%s,%s%s",
						field == 0b10000 ? "vcpop.m" : "vfirst.m",
						RVPRINT::reg(vd), RVPRINT::vreg(vs2), vmask(vm));
				return bad(b, n, i);
			case 0b010010: {
				// The integer extensions, whose source field spells the
				// factor with its low bit picking sign- over zero-extension,
				// plus the Zvbb bit operations that share the group.
				static const char* const names[16] = {
					nullptr, nullptr, "vzext.vf8", "vsext.vf8",
					"vzext.vf4", "vsext.vf4", "vzext.vf2", "vsext.vf2",
					"vbrev8.v", "vrev8.v", "vbrev.v", nullptr,
					"vclz.v", "vctz.v", "vcpop.v", nullptr,
				};
				if (form != AS_VV || field >= 16 || names[field] == nullptr)
					return bad(b, n, i);
				return unary(b, n, names[field], i);
			}
			case 0b010100:
				// The mask scans, plus the two index generators.
				if (form != AS_VV)
					return bad(b, n, i);
				switch (field) {
				case 0b00001: return unary(b, n, "vmsbf.m", i);
				case 0b00010: return unary(b, n, "vmsof.m", i);
				case 0b00011: return unary(b, n, "vmsif.m", i);
				case 0b10000: return unary(b, n, "viota.m", i);
				case 0b10001:
					// VID.V writes the element index, so it reads nothing.
					if (vs2 != 0)
						return bad(b, n, i);
					return snprintf(b, n, "vid.v\t%s%s",
						RVPRINT::vreg(vd), vmask(vm));
				}
				return bad(b, n, i);
			}

			const Entry& op = table[f6];
			if (op.name == nullptr || !(op.forms & form))
				return bad(b, n, i);
			return shaped(b, n, i, op, form, false);
		}

		// ---- OPFVV / OPFVF -------------------------------------------------

		static int op_float(char* b, size_t n, instr_t i, uint8_t form) noexcept
		{
			static const Entry table[64] = {
			/* 00 */ { "vfadd",       AS_VV|AS_VF, SH_ARITH },
			/* 01 */ { "vfredusum",   AS_VV,       SH_REDUCE },
			/* 02 */ { "vfsub",       AS_VV|AS_VF, SH_ARITH },
			/* 03 */ { "vfredosum",   AS_VV,       SH_REDUCE },
			/* 04 */ { "vfmin",       AS_VV|AS_VF, SH_ARITH },
			/* 05 */ { "vfredmin",    AS_VV,       SH_REDUCE },
			/* 06 */ { "vfmax",       AS_VV|AS_VF, SH_ARITH },
			/* 07 */ { "vfredmax",    AS_VV,       SH_REDUCE },
			/* 08 */ { "vfsgnj",      AS_VV|AS_VF, SH_ARITH },
			/* 09 */ { "vfsgnjn",     AS_VV|AS_VF, SH_ARITH },
			/* 10 */ { "vfsgnjx",     AS_VV|AS_VF, SH_ARITH },
			/* 11 */ { nullptr,       0,           SH_ARITH },
			/* 12 */ { nullptr,       0,           SH_ARITH },
			/* 13 */ { nullptr,       0,           SH_ARITH },
			/* 14 */ { "vfslide1up",        AS_VF, SH_ARITH },
			/* 15 */ { "vfslide1down",      AS_VF, SH_ARITH },
			/* 16 */ { nullptr,       0,           SH_ARITH }, // VFMV.F.S / VFMV.S.F
			/* 17 */ { nullptr,       0,           SH_ARITH },
			/* 18 */ { nullptr,       0,           SH_ARITH }, // the conversions
			/* 19 */ { nullptr,       0,           SH_ARITH }, // VFSQRT and friends
			/* 20 */ { nullptr,       0,           SH_ARITH },
			/* 21 */ { nullptr,       0,           SH_ARITH },
			/* 22 */ { nullptr,       0,           SH_ARITH },
			/* 23 */ { nullptr,       0,           SH_ARITH }, // VFMERGE / VFMV.V.F
			/* 24 */ { "vmfeq",       AS_VV|AS_VF, SH_ARITH },
			/* 25 */ { "vmfle",       AS_VV|AS_VF, SH_ARITH },
			/* 26 */ { nullptr,       0,           SH_ARITH },
			/* 27 */ { "vmflt",       AS_VV|AS_VF, SH_ARITH },
			/* 28 */ { "vmfne",       AS_VV|AS_VF, SH_ARITH },
			/* 29 */ { "vmfgt",             AS_VF, SH_ARITH },
			/* 30 */ { nullptr,       0,           SH_ARITH },
			/* 31 */ { "vmfge",             AS_VF, SH_ARITH },
			/* 32 */ { "vfdiv",       AS_VV|AS_VF, SH_ARITH },
			/* 33 */ { "vfrdiv",            AS_VF, SH_ARITH },
			/* 34 */ { nullptr,       0,           SH_ARITH },
			/* 35 */ { nullptr,       0,           SH_ARITH },
			/* 36 */ { "vfmul",       AS_VV|AS_VF, SH_ARITH },
			/* 37 */ { nullptr,       0,           SH_ARITH },
			/* 38 */ { nullptr,       0,           SH_ARITH },
			/* 39 */ { "vfrsub",            AS_VF, SH_ARITH },
			/* 40 */ { "vfmadd",      AS_VV|AS_VF, SH_MADD },
			/* 41 */ { "vfnmadd",     AS_VV|AS_VF, SH_MADD },
			/* 42 */ { "vfmsub",      AS_VV|AS_VF, SH_MADD },
			/* 43 */ { "vfnmsub",     AS_VV|AS_VF, SH_MADD },
			/* 44 */ { "vfmacc",      AS_VV|AS_VF, SH_MADD },
			/* 45 */ { "vfnmacc",     AS_VV|AS_VF, SH_MADD },
			/* 46 */ { "vfmsac",      AS_VV|AS_VF, SH_MADD },
			/* 47 */ { "vfnmsac",     AS_VV|AS_VF, SH_MADD },
			/* 48 */ { "vfwadd",      AS_VV|AS_VF, SH_ARITH },
			/* 49 */ { "vfwredusum",  AS_VV,       SH_REDUCE },
			/* 50 */ { "vfwsub",      AS_VV|AS_VF, SH_ARITH },
			/* 51 */ { "vfwredosum",  AS_VV,       SH_REDUCE },
			/* 52 */ { "vfwadd",      AS_VV|AS_VF, SH_WIDE },
			/* 53 */ { nullptr,       0,           SH_ARITH },
			/* 54 */ { "vfwsub",      AS_VV|AS_VF, SH_WIDE },
			/* 55 */ { nullptr,       0,           SH_ARITH },
			/* 56 */ { "vfwmul",      AS_VV|AS_VF, SH_ARITH },
			/* 57 */ { nullptr,       0,           SH_ARITH },
			/* 58 */ { nullptr,       0,           SH_ARITH },
			/* 59 */ { nullptr,       0,           SH_ARITH },
			/* 60 */ { "vfwmacc",     AS_VV|AS_VF, SH_MADD },
			/* 61 */ { "vfwnmacc",    AS_VV|AS_VF, SH_MADD },
			/* 62 */ { "vfwmsac",     AS_VV|AS_VF, SH_MADD },
			/* 63 */ { "vfwnmsac",    AS_VV|AS_VF, SH_MADD },
			};

			// The conversions, indexed by the source field. Widening reads a
			// half-width element (.v), narrowing a double-width one (.w).
			static const char* const conversions[32] = {
				"vfcvt.xu.f.v",      "vfcvt.x.f.v",
				"vfcvt.f.xu.v",      "vfcvt.f.x.v",
				nullptr,             nullptr,
				"vfcvt.rtz.xu.f.v",  "vfcvt.rtz.x.f.v",
				"vfwcvt.xu.f.v",     "vfwcvt.x.f.v",
				"vfwcvt.f.xu.v",     "vfwcvt.f.x.v",
				"vfwcvt.f.f.v",      nullptr,
				"vfwcvt.rtz.xu.f.v", "vfwcvt.rtz.x.f.v",
				"vfncvt.xu.f.w",     "vfncvt.x.f.w",
				"vfncvt.f.xu.w",     "vfncvt.f.x.w",
				"vfncvt.f.f.w",      "vfncvt.rod.f.f.w",
				"vfncvt.rtz.xu.f.w", "vfncvt.rtz.x.f.w",
				nullptr, nullptr, nullptr, nullptr,
				nullptr, nullptr, nullptr, nullptr,
			};

			const rv32v_instruction v { i };
			const uint32_t f6 = v.OPVV.funct6, vd = v.OPVV.vd;
			const uint32_t vs2 = v.OPVV.vs2, field = v.OPVV.vs1, vm = v.OPVV.vm;

			switch (f6) {
			case 0b010000:
				// The scalar moves, neither of which is ever masked.
				if (!vm)
					return bad(b, n, i);
				if (form == AS_VF) {
					if (vs2 != 0)
						return bad(b, n, i);
					return snprintf(b, n, "vfmv.s.f\t%s,%s",
						RVPRINT::vreg(vd), RVPRINT::freg(field));
				}
				if (field != 0)
					return bad(b, n, i);
				return snprintf(b, n, "vfmv.f.s\t%s,%s",
					RVPRINT::freg(vd), RVPRINT::vreg(vs2));
			case 0b010010:
				if (form != AS_VV || conversions[field] == nullptr)
					return bad(b, n, i);
				return unary(b, n, conversions[field], i);
			case 0b010011:
				if (form != AS_VV)
					return bad(b, n, i);
				switch (field) {
				case 0b00000: return unary(b, n, "vfsqrt.v", i);
				case 0b00100: return unary(b, n, "vfrsqrt7.v", i);
				case 0b00101: return unary(b, n, "vfrec7.v", i);
				case 0b10000: return unary(b, n, "vfclass.v", i);
				}
				return bad(b, n, i);
			case 0b010111:
				// VFMERGE and VFMV.V.F, as in the integer case: the move is
				// the unmasked form and has no vs2 to name.
				if (form != AS_VF)
					return bad(b, n, i);
				if (!vm)
					return snprintf(b, n, "vfmerge.vfm\t%s,%s,%s,v0",
						RVPRINT::vreg(vd), RVPRINT::vreg(vs2), RVPRINT::freg(field));
				if (vs2 != 0)
					return bad(b, n, i);
				return snprintf(b, n, "vfmv.v.f\t%s,%s",
					RVPRINT::vreg(vd), RVPRINT::freg(field));
			}

			const Entry& op = table[f6];
			if (op.name == nullptr || !(op.forms & form))
				return bad(b, n, i);
			return shaped(b, n, i, op, form, true);
		}

		// ---- vector configuration ------------------------------------------

		/// The vtype operand of a vsetvli/vsetivli, as `e8,m1,tu,mu`. Returns
		/// false for the encodings binutils prints as a bare number instead:
		/// the reserved LMUL, the element widths above 64, and any of the
		/// reserved high bits set.
		static bool vtype(char* buf, size_t len, uint32_t bits) noexcept
		{
			static const char* const lmul[8] =
				{ "m1", "m2", "m4", "m8", nullptr, "mf8", "mf4", "mf2" };
			const uint32_t vlmul = bits & 7, vsew = (bits >> 3) & 7;
			if (lmul[vlmul] == nullptr || vsew > 3 || (bits >> 8) != 0)
				return false;
			snprintf(buf, len, "e%u,%s,%s,%s", 8u << vsew, lmul[vlmul],
				(bits & 0x40) ? "ta" : "tu", (bits & 0x80) ? "ma" : "mu");
			return true;
		}

		/// VSETVLI, VSETIVLI and VSETVL, told apart by the top two bits.
		static int op_vsetvl(char* b, size_t n, instr_t i) noexcept
		{
			const rv32v_instruction v { i };
			char type[32];

			switch (i.whole >> 30) {
			case 0b11: { // VSETIVLI: a 5-bit AVL immediate, 10-bit vtype
				const uint32_t bits = (i.whole >> 20) & 0x3FF;
				if (!vtype(type, sizeof(type), bits))
					snprintf(type, sizeof(type), "%u", bits);
				return snprintf(b, n, "vsetivli\t%s,%u,%s",
					RVPRINT::reg(v.IVLI.rd), v.IVLI.uimm, type);
			}
			case 0b10: // VSETVL: vtype comes out of a register
				if (((i.whole >> 25) & 0x1F) != 0)
					return bad(b, n, i);
				return snprintf(b, n, "vsetvl\t%s,%s,%s",
					RVPRINT::reg(v.VSETVL.rd), RVPRINT::reg(v.VSETVL.rs1),
					RVPRINT::reg(v.VSETVL.rs2));
			default: { // VSETVLI: 11-bit vtype immediate
				const uint32_t bits = (i.whole >> 20) & 0x7FF;
				if (!vtype(type, sizeof(type), bits))
					snprintf(type, sizeof(type), "%u", bits);
				return snprintf(b, n, "vsetvli\t%s,%s,%s",
					RVPRINT::reg(v.VLI.rd), RVPRINT::reg(v.VLI.rs1), type);
			}
			}
		}

		// ---- vector loads and stores ---------------------------------------

		/// The element width a load or store names. Zero for the width field
		/// values that belong to the scalar FP loads instead.
		static unsigned eew(uint32_t width) noexcept
		{
			switch (width) {
				case 0b000: return 8;
				case 0b101: return 16;
				case 0b110: return 32;
				case 0b111: return 64;
			}
			return 0;
		}

		/**
		 * Vector LOAD-FP (0x07) and STORE-FP (0x27).
		 *
		 * mop picks the addressing mode, and nf the segment length, so most of
		 * the mnemonic is assembled rather than looked up: `vl`, an optional
		 * `seg<n>`, the width, and for the segmented indexed forms an `ei`
		 * that trades places with the segment count.
		 */
		static int op_memory(char* b, size_t n, instr_t i, bool is_store) noexcept
		{
			const rv32v_instruction v { i };
			const unsigned e = eew(v.VL.width);
			const unsigned nf = v.VL.nf, seg = nf + 1;
			const uint32_t vm = v.VL.vm;
			// MEW would widen the element past 64 bits, which no profile has.
			if (e == 0 || v.VL.mew)
				return bad(b, n, i);

			const char* const l = is_store ? "vs" : "vl";
			const char* const data = RVPRINT::vreg(v.VL.vd); // vs3 in a store
			const char* const base = RVPRINT::reg(v.VL.rs1);
			const char* const mask = vmask(vm);

			switch (v.VL.mop) {
			case 0b00: // unit-stride, with the sub-mode in the source field
				switch (v.VL.lumop) {
				case 0b00000:
					if (nf == 0)
						return snprintf(b, n, "%se%u.v\t%s,(%s)%s", l, e, data, base, mask);
					return snprintf(b, n, "%sseg%ue%u.v\t%s,(%s)%s",
						l, seg, e, data, base, mask);
				case 0b01000:
					// Whole-register: no vl, no mask, and the segment field is
					// the register count instead.
					if (!vm || (seg != 1 && seg != 2 && seg != 4 && seg != 8))
						return bad(b, n, i);
					// The store moves raw bytes, so it names no element width.
					if (is_store) {
						if (v.VL.width != 0)
							return bad(b, n, i);
						return snprintf(b, n, "vs%ur.v\t%s,(%s)", seg, data, base);
					}
					return snprintf(b, n, "vl%ure%u.v\t%s,(%s)", seg, e, data, base);
				case 0b01011:
					// The mask load and store move ceil(vl/8) bytes, always.
					if (!vm || nf != 0 || v.VL.width != 0)
						return bad(b, n, i);
					return snprintf(b, n, "%sm.v\t%s,(%s)", l, data, base);
				case 0b10000:
					// Fault-only-first, which only the load half has.
					if (is_store)
						return bad(b, n, i);
					if (nf == 0)
						return snprintf(b, n, "vle%uff.v\t%s,(%s)%s", e, data, base, mask);
					return snprintf(b, n, "vlseg%ue%uff.v\t%s,(%s)%s",
						seg, e, data, base, mask);
				}
				return bad(b, n, i);
			case 0b10: // strided: the stride is a register
				if (nf == 0)
					return snprintf(b, n, "%sse%u.v\t%s,(%s),%s%s",
						l, e, data, base, RVPRINT::reg(v.VLS.rs2), mask);
				return snprintf(b, n, "%ssseg%ue%u.v\t%s,(%s),%s%s",
					l, seg, e, data, base, RVPRINT::reg(v.VLS.rs2), mask);
			default: { // indexed: the width is the *index* width, not the data's
				const char* const order = (v.VL.mop == 0b01) ? "ux" : "ox";
				if (nf == 0)
					return snprintf(b, n, "%s%sei%u.v\t%s,(%s),%s%s",
						l, order, e, data, base, RVPRINT::vreg(v.VLX.vs2), mask);
				return snprintf(b, n, "%s%sseg%uei%u.v\t%s,(%s),%s%s",
					l, order, seg, e, data, base, RVPRINT::vreg(v.VLX.vs2), mask);
			}
			}
		}

		/// Guard against the decoder routing an encoding to the wrong handler:
		/// every OP-V printer covers only some of the funct3 values, so the
		/// ones it was given are spelled out and anything else says so.
		static int expect(char* b, size_t n, instr_t i,
			const char* family, uint32_t accepted) noexcept
		{
			const uint32_t funct3 = (i.whole >> 12) & 0x7;
			if ((accepted & (1u << funct3)) == 0)
				return snprintf(b, n,
					"%s\t<decoder routed a different instruction here>", family);
			switch (funct3) {
			case 0b000: return op_integer(b, n, i, AS_VV);
			case 0b100: return op_integer(b, n, i, AS_VX);
			case 0b011: return op_integer(b, n, i, AS_VI);
			case 0b010: return op_mask(b, n, i, AS_VV);
			case 0b110: return op_mask(b, n, i, AS_VX);
			case 0b001: return op_float(b, n, i, AS_VV);
			default:    return op_float(b, n, i, AS_VF);
			}
		}

		/// The same guard for the three vector-configuration instructions,
		/// which share funct3 and are told apart by the top two bits instead.
		static int expect_config(char* b, size_t n, instr_t i,
			const char* family, uint32_t accepted) noexcept
		{
			if (((i.whole >> 12) & 0x7) != 0b111
				|| (accepted & (1u << (i.whole >> 30))) == 0)
				return snprintf(b, n,
					"%s\t<decoder routed a different instruction here>", family);
			return op_vsetvl(b, n, i);
		}
	};
}
