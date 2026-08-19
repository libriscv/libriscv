#pragma once
#include "../common.hpp"
#include "../instruction_list.hpp"
#include "../rv32i_instr.hpp"
#include "../rvfd.hpp"
#include "../types.hpp"
#include "aj_api.hpp"
#include "aj_decode.hpp"

#include <vector>

namespace riscv
{
	template <int W> struct DecodedExecuteSegment;

	/// @brief Everything the emitter needs to know about the host machine layout.
	/// @details All displacements are relative to `CPU<W>&`, the first argument of
	/// every emitted function. They are measured from a live CPU instance rather
	/// than with offsetof(), which keeps them correct for any CPU<W> layout and
	/// for any Machine<W> the emitted code is later shared with.
	template <int W>
	struct AjInfo
	{
		int32_t reg_offset      = 0;   ///< &cpu.registers().get(0) - &cpu
		int32_t fpreg_offset    = 0;   ///< &cpu.registers().getfl(0) - &cpu
		int32_t arena_ptr       = 0;   ///< &memory.m_arena.data    - &cpu
		int32_t arena_rdbound   = 0;   ///< &memory.m_arena.read_boundary  - &cpu
		int32_t arena_wrbound   = 0;   ///< &memory.m_arena.write_boundary - &cpu
		int32_t arena_roend     = 0;   ///< &memory.m_arena.initial_rodata_end - &cpu
		/// @brief True when loads and stores may be inlined against the flat arena.
		bool inline_memory = false;
		const AjCallbacks<W>* cb = nullptr;
	};

	/// @brief True when the F/D extension may be emitted as native code.
	/// @details FCSR emulation tracks the exception flags of every arithmetic
	/// operation by recomputing it in a wider type and comparing, which no host
	/// instruction sequence reproduces. Under that build every FP instruction
	/// stays with the interpreter, and only ends a region.
	static constexpr bool aj_fpu_emittable = !riscv::fcsr_emulation;

	/// @brief True when this host can compute a fused multiply-add.
	/// @details RISC-V requires FMADD and its three siblings to round once. A
	/// separate multiply and add rounds twice, so on a host without FMA those
	/// four opcodes end the region and the interpreter's std::fma runs instead.
	/// Defined in aj_emit.cpp, where the asmjit headers are available.
	bool aj_host_has_fma() noexcept;

	/// @brief One bit-manipulation operation, as the emitter thinks of it.
	/// @details Covers Zba, Zbb and Zbs, plus the PACK family that ZEXT.H is the
	/// rs2 == 0 form of. Zbc's carry-less multiplies are deliberately absent:
	/// they have no compact host sequence, and compilers essentially never emit
	/// them, so they keep ending a region the way every unhandled encoding does.
	enum class AjZb : uint8_t
	{
		kNone = 0,
		// --- Zba: address generation ---
		kShAdd,     ///< SH1ADD, SH2ADD, SH3ADD           (the scale is in funct3)
		kShAddUw,   ///< SH1ADD.UW, SH2ADD.UW, SH3ADD.UW  (RV64)
		kAddUw,     ///< ADD.UW                           (RV64)
		kSlliUw,    ///< SLLI.UW                          (RV64)
		// --- Zbb: logic with negate ---
		kAndn, kOrn, kXnor,
		// --- Zbb: min and max ---
		kMin, kMinu, kMax, kMaxu,
		// --- Zbb: bit counting ---
		kClz, kCtz, kCpop,
		kClzw, kCtzw, kCpopw,          ///< RV64
		// --- Zbb: extend, reverse, combine ---
		kSextB, kSextH, kRev8, kOrcB,
		kPack, kPackH,                 ///< ZEXT.H on RV32 is PACK with rs2 == 0
		kPackW,                        ///< RV64; ZEXT.H is its rs2 == 0 form
		// --- Zbb: rotate ---
		kRol, kRor, kRori,
		kRolw, kRorw, kRoriw,          ///< RV64
		// --- Zbs: single-bit ---
		kBset, kBclr, kBinv, kBext,
		kBseti, kBclri, kBinvi, kBexti,
	};

	/// @brief Classifies one instruction as a bit-manipulation operation.
	/// @details This is the single source of truth for the Zb encodings: both
	/// aj_is_emittable() and the emitter dispatch on it, so region discovery can
	/// never claim an encoding that emission then quietly skips -- which would
	/// leave rd holding a stale value rather than faulting.
	/// @tparam W The guest register width. RV64 adds the *W and *.UW forms, and
	/// widens the shift amount that a shamt-carrying encoding may hold.
	template <int W>
	inline AjZb aj_zb_classify(rv32i_instruction i) noexcept
	{
		constexpr bool RV64 = (W == 8);
		// On RV32 the shift amount is five bits wide and bit 5 belongs to funct7,
		// so every shamt-carrying encoding that sets it is reserved instead.
		const bool shamt_ok = RV64 || (i.Itype.imm & 0x20) == 0;

		switch (i.opcode())
		{
		case RV32I_OP:
			// The key is funct3 | funct7 << 4, the same one the interpreter's
			// jumptable_friendly_op() builds, so rvi_instr.cpp can be read
			// alongside this table.
			switch (i.Rtype.jumptable_friendly_op()) {
			case 0x44:  return AjZb::kPack;    // ZEXT.H on RV32 when rs2 == 0
			case 0x47:  return AjZb::kPackH;
			case 0x54:  return AjZb::kMin;
			case 0x55:  return AjZb::kMinu;
			case 0x56:  return AjZb::kMax;
			case 0x57:  return AjZb::kMaxu;
			case 0x102: case 0x104: case 0x106:
				return AjZb::kShAdd;
			case 0x141: return AjZb::kBset;
			case 0x204: return AjZb::kXnor;
			case 0x206: return AjZb::kOrn;
			case 0x207: return AjZb::kAndn;
			case 0x241: return AjZb::kBclr;
			case 0x245: return AjZb::kBext;
			case 0x301: return AjZb::kRol;
			case 0x305: return AjZb::kRor;
			case 0x341: return AjZb::kBinv;
			default:    return AjZb::kNone;
			}

		case RV32I_OP_IMM:
			if (i.Itype.funct3 == 0x1) {
				// The unary forms name no shift amount at all, so the whole
				// 12-bit immediate identifies them.
				switch (i.Itype.imm) {
				case 0x600: return AjZb::kClz;
				case 0x601: return AjZb::kCtz;
				case 0x602: return AjZb::kCpop;
				case 0x604: return AjZb::kSextB;
				case 0x605: return AjZb::kSextH;
				default: break;
				}
				if (!shamt_ok) return AjZb::kNone;
				switch (i.Itype.high_bits()) {
				case 0x280: return AjZb::kBseti;
				case 0x480: return AjZb::kBclri;
				case 0x680: return AjZb::kBinvi;
				default:    return AjZb::kNone;
				}
			}
			if (i.Itype.funct3 == 0x5) {
				// ORC.B and REV8 carry no shift amount either, so they are
				// matched before the RV32 shamt rule can reject them.
				if (i.Itype.imm == 0x287)         return AjZb::kOrcB;
				if (i.Itype.template is_rev8<W>()) return AjZb::kRev8;
				if (!shamt_ok) return AjZb::kNone;
				if (i.Itype.high_bits() == 0x600) return AjZb::kRori;
				if (i.Itype.high_bits() == 0x480) return AjZb::kBexti;
			}
			return AjZb::kNone;

		case RV64I_OP_IMM32:
			if constexpr (RV64) {
				if (i.Itype.funct3 == 0x1) {
					switch (i.Itype.imm) {
					case 0x600: return AjZb::kClzw;
					case 0x601: return AjZb::kCtzw;
					case 0x602: return AjZb::kCpopw;
					default: break;
					}
					// SLLI.UW is the one OP-IMM-32 form with a full six-bit
					// shift amount, because it shifts a 64-bit value.
					if (i.Itype.high_bits() == 0x080) return AjZb::kSlliUw;
				}
				else if (i.Itype.funct3 == 0x5) {
					// RORIW rotates 32 bits, so its shift amount is five bits
					// wide even here.
					if (i.Itype.high_bits() == 0x600 && (i.Itype.imm & 0x20) == 0)
						return AjZb::kRoriw;
				}
			}
			return AjZb::kNone;

		case RV64I_OP32:
			if constexpr (RV64) {
				switch (i.Rtype.jumptable_friendly_op()) {
				case 0x40:  return AjZb::kAddUw;
				case 0x44:  return AjZb::kPackW;   // ZEXT.H when rs2 == 0
				case 0x102: case 0x104: case 0x106:
					return AjZb::kShAddUw;
				case 0x301: return AjZb::kRolw;
				case 0x305: return AjZb::kRorw;
				default:    break;
				}
			}
			return AjZb::kNone;

		default:
			return AjZb::kNone;
		}
	}

	/// @brief True if the instruction can be emitted by the asmjit backend.
	/// Anything that is false terminates the region.
	/// @details This predicate is shared between region discovery and emission,
	/// so that the two can never disagree about where a region ends. It must
	/// stay an exact mirror of the opcodes handled in aj_emit.cpp.
	/// @tparam W The guest register width. RV64 adds LD/LWU/SD and the *W
	/// opcodes, widens the shift amount that a shift-immediate may carry, and
	/// is the only width with the 64-bit FCVT forms and FMV.X.D/FMV.D.X.
	template <int W>
	inline bool aj_is_emittable(rv32i_instruction i) noexcept
	{
		// The shift-amount field is 5 bits on RV32 and 6 bits on RV64, so the
		// number of bits left over to identify the operation differs.
		constexpr uint32_t SHMASK = (W == 8) ? 0xFC0 : 0xFE0;

		switch (i.opcode())
		{
		case RV32I_LUI:
		case RV32I_AUIPC:
		case RV32I_JAL:
			return true;
		case RV32I_FENCE:
			// MISC-MEM carries more than FENCE: funct3 1 is FENCE.I, which the
			// interpreter uses to invalidate stale execute segments, and funct3 2
			// is the Zicbo group, whose CBO.ZERO writes 64 bytes of zeroes. A
			// barrier is the right answer for plain FENCE only.
			return i.Itype.funct3 == 0x0;
		case RV32I_JALR:
			return i.Itype.funct3 == 0x0;
		case RV32I_BRANCH:
			switch (i.Btype.funct3) {
			case 0x0: case 0x1: case 0x4: case 0x5: case 0x6: case 0x7:
				return true;
			default: // 0x2 and 0x3 are not valid branch encodings
				return false;
			}
		case RV32I_LOAD:
			switch (i.Itype.funct3) {
			case 0x0: case 0x1: case 0x2: case 0x4: case 0x5:
				return true;   // LB LH LW LBU LHU
			case 0x3: case 0x6:
				return W == 8; // LD, LWU
			default:
				return false;
			}
		case RV32I_STORE:
			return i.Stype.funct3 <= (W == 8 ? 0x3u : 0x2u);   // SB SH SW (SD)
		case RV32I_OP_IMM:
			switch (i.Itype.funct3) {
			case 0x1: // SLLI, or one of the Zb* encodings sharing funct3
				if ((i.Itype.imm & SHMASK) == 0x000)
					return true;
				break;
			case 0x5: // SRLI / SRAI, or RORI and friends
				if ((i.Itype.imm & SHMASK) == 0x000
					|| (i.Itype.imm & SHMASK) == 0x400)
					return true;
				break;
			default: // ADDI, SLTI, SLTIU, XORI, ORI, ANDI
				return true;
			}
			return aj_zb_classify<W>(i) != AjZb::kNone;
		case RV32I_OP:
			if (i.Rtype.funct7 == 0b0000000)
				return true;  // ADD SLL SLT SLTU XOR SRL OR AND
			if (i.Rtype.funct7 == 0b0100000
				&& (i.Rtype.funct3 == 0x0 || i.Rtype.funct3 == 0x5))
				return true;  // SUB, SRA -- the rest of funct7 0x20 is Zbb
			if (i.Rtype.funct7 == 0b0000001)
				return true;  // M: MUL MULH MULHSU MULHU DIV DIVU REM REMU
			return aj_zb_classify<W>(i) != AjZb::kNone;
		case RV64I_OP_IMM32:
			if constexpr (W == 8) {
				switch (i.Itype.funct3) {
				case 0x0: // ADDIW
					return true;
				case 0x1: // SLLIW, always a 5-bit shift amount
					if ((i.Itype.imm & 0xFE0) == 0x000)
						return true;
					break;
				case 0x5: // SRLIW / SRAIW
					if ((i.Itype.imm & 0xFE0) == 0x000
						|| (i.Itype.imm & 0xFE0) == 0x400)
						return true;
					break;
				default:
					break;
				}
				return aj_zb_classify<W>(i) != AjZb::kNone;
			}
			return false;
		case RV64I_OP32:
			if constexpr (W == 8) {
				if (i.Rtype.funct7 == 0b0000000
					&& (i.Rtype.funct3 == 0x0     // ADDW
						|| i.Rtype.funct3 == 0x1  // SLLW
						|| i.Rtype.funct3 == 0x5))// SRLW
					return true;
				if (i.Rtype.funct7 == 0b0100000
					&& (i.Rtype.funct3 == 0x0     // SUBW
						|| i.Rtype.funct3 == 0x5))// SRAW
					return true;
				if (i.Rtype.funct7 == 0b0000001
					&& (i.Rtype.funct3 == 0x0     // MULW
						|| i.Rtype.funct3 >= 0x4))// DIVW DIVUW REMW REMUW
					return true;
				return aj_zb_classify<W>(i) != AjZb::kNone;
			}
			return false;

		// --- F/D extension ---
		case RV32F_LOAD:   // FLW, FLD
		case RV32F_STORE:  // FSW, FSD
			// funct3 is the access width, in the same bits either way. Both
			// widths carry FLD/FSD: RV32D moves doubles through a machine
			// whose integer registers are half that size.
			return aj_fpu_emittable
				&& (i.Itype.funct3 == 0x2 || i.Itype.funct3 == 0x3);
		case RV32F_FMADD:
		case RV32F_FMSUB:
		case RV32F_FNMADD:
		case RV32F_FNMSUB:
			// funct2 is the precision: 0 is single, 1 is double, and the
			// half- and quad-precision encodings are not implemented at all.
			return aj_fpu_emittable && aj_host_has_fma()
				&& rv32f_instruction(i).R4type.funct2 <= 0x1;
		case RV32F_FPFUNC: {
			if (!aj_fpu_emittable)
				return false;
			const rv32f_instruction fi { i };
			if (fi.R4type.funct2 > 0x1)
				return false;
			const bool is_double = (fi.R4type.funct2 == 0x1);
			switch (i.fpfunc()) {
			case RV32F__FADD: case RV32F__FSUB:
			case RV32F__FMUL: case RV32F__FDIV:
				return true;
			case RV32F__FSQRT:
				// rs2 is not a register here; every value but 0 is reserved.
				return fi.R4type.rs2 == 0x0;
			case RV32F__FCVT_SD_DS:
				// rs2 names the *source* format, where funct2 names the
				// destination: 0 is single, 1 is double, 2 is half (Zfhmin) and
				// 4/5 are Zfa's FROUND/FROUNDNX, which are not conversions at
				// all. Only a real S<->D conversion may be emitted.
				return fi.R4type.rs2 <= 0x1 && fi.R4type.rs2 != fi.R4type.funct2;
			case RV32F__FSGNJ_NX:
				return fi.R4type.funct3 <= 0x2;
			case RV32F__FMIN_MAX:
				return fi.R4type.funct3 <= 0x1;
			case RV32F__FEQ_LT_LE:
				return fi.R4type.funct3 <= 0x2;
			case RV32F__FCVT_W_SD:   // FCVT.{W,WU,L,LU}.{S,D}
			case RV32F__FCVT_SD_W:   // FCVT.{S,D}.{W,WU,L,LU}
				// The 64-bit integer forms only exist on RV64.
				return fi.R4type.rs2 <= (W == 8 ? 0x3u : 0x1u);
			case RV32F__FMV_X_W:
				// funct3 0 is FMV.X.W / FMV.X.D, funct3 1 is FCLASS. Only the
				// move of a whole double into an integer register needs RV64;
				// FCLASS reads the bit fields and fits a 32-bit result.
				// rs2 must be 0: Zfa reuses rs2 1 for FMVH.X.D.
				if (fi.R4type.rs2 != 0x0)
					return false;
				if (fi.R4type.funct3 == 0x1)
					return true;
				return fi.R4type.funct3 == 0x0 && (!is_double || W == 8);
			case RV32F__FMV_W_X:
				// rs2 must be 0: Zfa's FLI.{S,D} shares this funct7 with rs2 1,
				// and reads a constant table index out of rs1 rather than a
				// register. Emitting it as FMV.W.X silently loads garbage.
				return fi.R4type.rs2 == 0x0
					&& fi.R4type.funct3 == 0x0 && (!is_double || W == 8);
			default:
				return false;
			}
		}

		default:
			// SYSTEM (ECALL/EBREAK/CSR) and the A/V opcodes all terminate the
			// region: the interpreter picks them up at the address we exit with.
			return false;
		}
	}
	template <int W>
	inline bool aj_is_emittable(const AjDecoded& d) noexcept
	{
		return d.valid && aj_is_emittable<W>(d.instr);
	}

	/// @brief True when the instruction never falls through to the next address.
	inline bool aj_is_unconditional_jump(rv32i_instruction i) noexcept
	{
		return i.opcode() == RV32I_JAL || i.opcode() == RV32I_JALR;
	}

	/// @brief Emits one region as a single host function.
	/// @param entry The guest address the emitted function is entered at.
	/// @param instrs The region's reachable instruction addresses, ascending.
	/// @return nullptr if the region could not be emitted for any reason.
	/// @details Defined in aj_emit.cpp.
	template <int W>
	aj_block_func<W> aj_emit_region(AjCode&, const MachineOptions<W>&,
		const DecodedExecuteSegment<W>&, const AjInfo<W>&,
		address_type<W> entry, const std::vector<address_type<W>>& instrs);
}
