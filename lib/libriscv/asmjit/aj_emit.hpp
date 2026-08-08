#pragma once
#include "../common.hpp"
#include "../instruction_list.hpp"
#include "../rv32i_instr.hpp"
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
		int32_t arena_ptr       = 0;   ///< &memory.m_arena.data    - &cpu
		int32_t arena_rdbound   = 0;   ///< &memory.m_arena.read_boundary  - &cpu
		int32_t arena_wrbound   = 0;   ///< &memory.m_arena.write_boundary - &cpu
		int32_t arena_roend     = 0;   ///< &memory.m_arena.initial_rodata_end - &cpu
		/// @brief True when loads and stores may be inlined against the flat arena.
		bool inline_memory = false;
		const AjCallbacks<W>* cb = nullptr;
	};

	/// @brief True if the instruction can be emitted by the asmjit backend.
	/// Anything that is false terminates the region.
	/// @details This predicate is shared between region discovery and emission,
	/// so that the two can never disagree about where a region ends. It must
	/// stay an exact mirror of the opcodes handled in aj_emit.cpp.
	/// @tparam W The guest register width. RV64 adds LD/LWU/SD and the *W
	/// opcodes, and widens the shift amount that a shift-immediate may carry.
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
		case RV32I_FENCE:
			return true;
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
			case 0x1: // SLLI, but not the Zb* encodings sharing funct3
				return (i.Itype.imm & SHMASK) == 0x000;
			case 0x5: // SRLI / SRAI, but not RORI and friends
				return (i.Itype.imm & SHMASK) == 0x000
					|| (i.Itype.imm & SHMASK) == 0x400;
			default: // ADDI, SLTI, SLTIU, XORI, ORI, ANDI
				return true;
			}
		case RV32I_OP:
			if (i.Rtype.funct7 == 0b0000000)
				return true;  // ADD SLL SLT SLTU XOR SRL OR AND
			if (i.Rtype.funct7 == 0b0100000)
				return i.Rtype.funct3 == 0x0 || i.Rtype.funct3 == 0x5; // SUB, SRA
			return false;     // M-extension and the bit-manipulation encodings
		case RV64I_OP_IMM32:
			if constexpr (W == 8) {
				switch (i.Itype.funct3) {
				case 0x0: // ADDIW
					return true;
				case 0x1: // SLLIW, always a 5-bit shift amount
					return (i.Itype.imm & 0xFE0) == 0x000;
				case 0x5: // SRLIW / SRAIW
					return (i.Itype.imm & 0xFE0) == 0x000
						|| (i.Itype.imm & 0xFE0) == 0x400;
				default:
					return false;
				}
			}
			return false;
		case RV64I_OP32:
			if constexpr (W == 8) {
				if (i.Rtype.funct7 == 0b0000000)
					return i.Rtype.funct3 == 0x0    // ADDW
						|| i.Rtype.funct3 == 0x1    // SLLW
						|| i.Rtype.funct3 == 0x5;   // SRLW
				if (i.Rtype.funct7 == 0b0100000)
					return i.Rtype.funct3 == 0x0    // SUBW
						|| i.Rtype.funct3 == 0x5;   // SRAW
			}
			return false;     // MULW/DIVW and the bit-manipulation encodings
		default:
			// SYSTEM (ECALL/EBREAK/CSR) and the F/D/A/V opcodes all terminate the
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
