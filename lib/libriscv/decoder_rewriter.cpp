#include "machine.hpp"
#include "decoder_cache.hpp"
#include "instruction_list.hpp"
#include "rv32i_instr.hpp"
#include "rvc.hpp"
#include "instr_helpers.hpp"

namespace riscv
{
	static const unsigned PCAL= compressed_enabled ? 2 : 4;
	static const unsigned PCALBITS = compressed_enabled ? 1 : 2;

	union MoveType {
		struct {
			uint16_t rs2;
			uint16_t rs1;
		};
		uint32_t whole;
	};
	static_assert(sizeof(MoveType) == 4, "is a 4-byte instruction");

	union OpType {
		struct {
			uint8_t rd;
			uint8_t rs1;
			uint8_t rs2;
			int8_t  imm;
		};
		uint32_t whole;
	};
	static_assert(sizeof(OpType) == 4, "is a 4-byte instruction");

	union FasterBtype {
		struct {
			uint8_t rs2;
			uint8_t rs1;
			int16_t imm;
		};
		uint32_t whole;
		int32_t signed_imm() const noexcept {
			return imm;
		}
	};
	static_assert(sizeof(FasterBtype) == 4, "is a 4-byte instruction");

	union ZeroBtype {
		struct {
			uint8_t rs1;
			int16_t imm;
		};
		uint32_t whole;
		int32_t signed_imm() const noexcept {
			return imm;
		}
	};
	static_assert(sizeof(ZeroBtype) == 4, "is a 4-byte instruction");

	union FasterStype {
		struct {
			uint8_t rsy;
			uint8_t rsx;
			int16_t imm;
		};
		uint32_t whole;
		int32_t signed_imm() const noexcept {
			return imm;
		}
	};
	static_assert(sizeof(FasterStype) == 4, "is a 4-byte instruction");

	union FasterJtype {
		struct {
			int32_t imm;
		};
		uint32_t whole;
	};
	static_assert(sizeof(FasterJtype) == 4, "is a 4-byte instruction");

	template <int W> static RVPRINTR_ATTR
	int rewritten_instr_printer(char* buffer, size_t len, const CPU<W>&, rv32i_instruction) {
		return snprintf(buffer, len, "Rewritten instruction");
	}

	template <typename T> static inline
	T& view_as(rv32i_instruction& i) {
		static_assert(sizeof(T) == sizeof(i), "Must be same size as instruction!");
		return *(T*) &i;
	}

	template <int W> static inline
	Instruction<W> rewritten_instruction(instruction_handler<W> func) {
		return {func, rewritten_instr_printer};
	}

	template <int W> RISCV_INTERNAL
	Instruction<W> CPU<W>::decode_rewrite(address_t pc, rv32i_instruction& instr)
	{
		using sign_type = typename std::make_signed<address_t>::type;
		const auto original = instr;
		(void) pc;

		// NOTE: Every rewritten instruction sets aside
		// the first 2 bits to preserve the instruction length
		if (original.length() == 4)
		switch (original.opcode()) {
		case RV32I_OP: {
			// Rtype.rd=0 is a no-op in all cases, and we only care about
			// ADD which is a very common instruction.
			if (LIKELY(original.Rtype.rd != 0)) {
				switch (original.Rtype.jumptable_friendly_op()) {
				case 0x0: { // OP_ADD
					OpType rewritten;
					rewritten.rd  = original.Rtype.rd;
					rewritten.rs1 = original.Rtype.rs1;
					rewritten.rs2 = original.Rtype.rs2;
					instr.whole = rewritten.whole;
					return rewritten_instruction<W>(
						[] (auto& cpu, auto instr) RVINSTR_ATTR {
							const auto& rop = view_as<OpType> (instr);
							cpu.reg(rop.rd) = cpu.reg(rop.rs1) + cpu.reg(rop.rs2);
						}); // OP_ADD
					}
				}
			} break;
		}
		case RV32I_OP_IMM: {
			// Itype.rd=0 is a no-op in all cases, and we only care about
			// ADDI which is the most used instruction of all.
			if (original.Itype.rd != 0 && original.Itype.funct3 == 0x0)
			{	// OP_IMM.ADDI
				if (original.Itype.rs1 == 0) { // LI
					ZeroBtype rewritten;
					rewritten.rs1 = original.Itype.rd;
					rewritten.imm = original.Itype.signed_imm();
					instr.whole = rewritten.whole;
					return rewritten_instruction<W>(
						[] (auto& cpu, auto instr) RVINSTR_ATTR {
							const auto& rop = view_as<ZeroBtype> (instr);
							// LI: Load sign-extended 12-bit immediate
							cpu.reg(rop.rs1) = (RVSIGNTYPE(cpu)) rop.imm;
						}); // OP_IMM.LI
				} else if (original.Itype.imm == 0) { // MV
					MoveType rewritten;
					rewritten.rs1 = original.Itype.rd;
					rewritten.rs2 = original.Itype.rs1;
					instr.whole = rewritten.whole;
					return rewritten_instruction<W>(
						[] (auto& cpu, auto instr) RVINSTR_ATTR {
							const auto& rop = view_as<MoveType> (instr);
							cpu.reg(rop.rs1) = cpu.reg(rop.rs2);
						}); // OP_IMM.MV
				} else {
					FasterBtype rewritten;
					rewritten.rs1 = original.Itype.rd;
					rewritten.rs2 = original.Itype.rs1;
					rewritten.imm = original.Itype.signed_imm();

					instr.whole = rewritten.whole;
					return rewritten_instruction<W>(
						[] (auto& cpu, auto instr) RVINSTR_ATTR {
							const auto& rop = view_as<FasterBtype> (instr);
							cpu.reg(rop.rs1) =
								(RVSIGNTYPE(cpu)) (cpu.reg(rop.rs2) + rop.imm);
						}); // OP_IMM.ADDI
				}
			}
			break;
			} // RV32I_OP_IMM
		case RV32I_BRANCH: {
			// Rewrite all B-type instructions to become faster.
			// Ignore unaligned branches. This will allow us to
			// jump unaligned with (mostly) all branch instructions.
			const auto bdest = pc + original.Btype.signed_imm() - 4;
			if ((bdest % PCAL) != 0) break;
			// We verify if the jump is within the execute segment
			// and if not, we fallback to the regular branch decoding
		#ifdef RISCV_INBOUND_JUMPS_ONLY
			if (UNLIKELY(!is_executable(bdest))) {
				break;
			}
		#endif

			if (original.Btype.rs2 == 0) {
				ZeroBtype rewritten;
				rewritten.rs1 = original.Btype.rs1;
				rewritten.imm = original.Btype.signed_imm() - 4;
				assert(original.Btype.rs1 == rewritten.rs1);
				assert(original.Btype.signed_imm()-4 == rewritten.signed_imm());
				instr.whole = rewritten.whole;
				switch (original.Btype.funct3) {
				case 0x0: // BRANCH_EQ
					return rewritten_instruction<W>(
						[] (auto& cpu, auto instr) RVINSTR_ATTR {
							const auto& rop = view_as<ZeroBtype> (instr);
							if (cpu.reg(rop.rs1) == 0) {
								cpu.registers().pc += rop.signed_imm();
							}
						}); // BEQ
				case 0x1: // BRANCH_NE
					return rewritten_instruction<W>(
						[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
							const auto& rop = view_as<ZeroBtype> (instr);
							if (cpu.reg(rop.rs1) != 0) {
								cpu.registers().pc += rop.signed_imm();
							}
						}); // BNE
				case 0x4: // BRANCH_LT
					return rewritten_instruction<W>(
						[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
							const auto& rop = view_as<ZeroBtype> (instr);
							if (sign_type(cpu.reg(rop.rs1)) < 0) {
								cpu.registers().pc += rop.signed_imm();
							}
						}); // BLT
				case 0x5: // BRANCH_GE
					return rewritten_instruction<W>(
						[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
							const auto& rop = view_as<ZeroBtype> (instr);
							if (sign_type(cpu.reg(rop.rs1)) >= 0) {
								cpu.registers().pc += rop.signed_imm();
							}
						}); // BGE
				default:
					// Restore original for invalid BRANCH instructions
					instr = original;
				} // BRANCH type
			} // rs2 == 0
			FasterBtype rewritten;
			rewritten.rs2 = original.Btype.rs2;
			rewritten.rs1 = original.Btype.rs1;
			rewritten.imm = original.Btype.signed_imm() - 4;
			assert(original.Btype.signed_imm()-4 == rewritten.signed_imm());
			assert(original.Btype.rs1 == rewritten.rs1);
			assert(original.Btype.rs2 == rewritten.rs2);

			switch (original.Btype.funct3) {
			case 0x0: // BRANCH_EQ
				instr.whole = rewritten.whole;
				return rewritten_instruction<W>(
					[] (auto& cpu, auto instr) RVINSTR_ATTR {
						const auto& rop = view_as<FasterBtype> (instr);
						if (cpu.reg(rop.rs1) == cpu.reg(rop.rs2)) {
							cpu.registers().pc += rop.signed_imm();
						}
					}); // BEQ
			case 0x1: // BRANCH_NE
				instr.whole = rewritten.whole;
				return rewritten_instruction<W>(
					[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
						const auto& rop = view_as<FasterBtype> (instr);
						if (cpu.reg(rop.rs1) != cpu.reg(rop.rs2)) {
							cpu.registers().pc += rop.signed_imm();
						}
					}); // BNE
			case 0x4: // BRANCH_LT
				instr.whole = rewritten.whole;
				return rewritten_instruction<W>(
					[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
						const auto& rop = view_as<FasterBtype> (instr);
						if (sign_type(cpu.reg(rop.rs1)) < sign_type(cpu.reg(rop.rs2))) {
							cpu.registers().pc += rop.signed_imm();
						}
					}); // BLT
			case 0x5: // BRANCH_GE
				instr.whole = rewritten.whole;
				return rewritten_instruction<W>(
					[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
						const auto& rop = view_as<FasterBtype> (instr);
						if (sign_type(cpu.reg(rop.rs1)) >= sign_type(cpu.reg(rop.rs2))) {
							cpu.registers().pc += rop.signed_imm();
						}
					}); // BGE
			case 0x6: // BRANCH_LTU
				instr.whole = rewritten.whole;
				return rewritten_instruction<W>(
					[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
						const auto& rop = view_as<FasterBtype> (instr);
						if (cpu.reg(rop.rs1) < cpu.reg(rop.rs2)) {
							cpu.registers().pc += rop.signed_imm();
						}
					}); // BLTU
			case 0x7: // BRANCH_GEU
				instr.whole = rewritten.whole;
				return rewritten_instruction<W>(
					[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
						const auto& rop = view_as<FasterBtype> (instr);
						if (cpu.reg(rop.rs1) >= cpu.reg(rop.rs2)) {
							cpu.registers().pc += rop.signed_imm();
						}
					}); // BGEU
			} // BRANCH type
			} // RV32I_BRANCH
			break;
		case RV32I_STORE:
			if (original.Stype.rs2 == 0) {
				// Accelerate store zero
				FasterStype rewritten;
				rewritten.rsx = original.Stype.rs1;
				rewritten.imm = original.Stype.signed_imm();
				assert(original.Stype.signed_imm() == rewritten.signed_imm());

				switch (original.Stype.funct3) {
				case 0x0: // STORE zero i8
					instr.whole = rewritten.whole;
					return rewritten_instruction<W>(
						[] (auto& cpu, auto instr) RVINSTR_ATTR {
							const auto& rop = view_as<FasterStype> (instr);
							cpu.machine().memory.template write<uint8_t>(
								cpu.reg(rop.rsx) + rop.imm, 0);
						}); // i8
				case 0x1: // STORE zero i16
					instr.whole = rewritten.whole;
					return rewritten_instruction<W>(
						[] (auto& cpu, auto instr) RVINSTR_ATTR {
							const auto& rop = view_as<FasterStype> (instr);
							cpu.machine().memory.template write<uint16_t>(
								cpu.reg(rop.rsx) + rop.imm, 0);
						}); // i16
				case 0x2: // STORE zero i32
					instr.whole = rewritten.whole;
					return rewritten_instruction<W>(
						[] (auto& cpu, auto instr) RVINSTR_ATTR {
							const auto& rop = view_as<FasterStype> (instr);
							cpu.machine().memory.template write<uint32_t>(
								cpu.reg(rop.rsx) + rop.imm, 0);
						}); // i32
				case 0x3: // STORE zero i64
					instr.whole = rewritten.whole;
					if constexpr (sizeof(address_t) >= 8) {
						return rewritten_instruction<W>(
						[] (auto& cpu, auto instr) RVINSTR_ATTR {
							const auto& rop = view_as<FasterStype> (instr);
							cpu.machine().memory.template write<uint64_t>(
								cpu.reg(rop.rsx) + rop.imm, 0);
						}); // i64
					} // 64-bit
				} // STORE func3
			}
			else if (original.Stype.signed_imm() != 0) {
				// Accelerate store imm
				FasterStype rewritten;
				rewritten.rsx = original.Stype.rs1;
				rewritten.rsy = original.Stype.rs2;
				rewritten.imm = original.Stype.signed_imm();
				assert(original.Stype.signed_imm() == rewritten.signed_imm());

				switch (original.Stype.funct3) {
				case 0x2: // STORE rs1+imm, i32
					instr.whole = rewritten.whole;
					return rewritten_instruction<W>(
						[] (auto& cpu, auto instr) RVINSTR_ATTR {
							const auto& rop = view_as<FasterStype> (instr);
							cpu.machine().memory.template write<uint32_t>(
								cpu.reg(rop.rsx) + RVIMM(cpu, rop), cpu.reg(rop.rsy));
						}); // i32
				case 0x3: // STORE rs1+imm, i64
					instr.whole = rewritten.whole;
					if constexpr (sizeof(address_t) >= 8) {
						return rewritten_instruction<W>(
						[] (auto& cpu, auto instr) RVINSTR_ATTR {
							const auto& rop = view_as<FasterStype> (instr);
							cpu.machine().memory.template write<uint64_t>(
								cpu.reg(rop.rsx) + RVIMM(cpu, rop), cpu.reg(rop.rsy));
						}); // i64
					} // 64-bit
				} // STORE func3
			} // RV32I_STORE
			break;
		case RV32I_JAL: {
			const auto addr = pc + original.Jtype.jump_offset() - 4;
			const bool is_aligned = addr % PCAL == 0;
			const bool below32 = addr < (uint64_t(1) << (32 + PCALBITS));
			// If we can't see that it's executable we leave it
			// NOTE: is_executable is only needed when inbound jumps only
			if (is_aligned && below32 && is_executable(addr)) {
				FasterJtype rewritten;
				rewritten.imm = addr >> PCALBITS;
				// Handle the common cases x0 and x1
				if (original.Jtype.rd == 0) {
					instr.whole = rewritten.whole;
					return rewritten_instruction<W>(
						[] (auto& cpu, auto instr) RVINSTR_ATTR {
							const auto& rop = view_as<FasterJtype> (instr);
							cpu.registers().pc = rop.imm << PCALBITS;
						}); // JAL zero, pc+imm
				} else if (original.Jtype.rd == REG_RA) {
					instr.whole = rewritten.whole;
					return rewritten_instruction<W>(
						[] (auto& cpu, auto instr) RVINSTR_ATTR {
							const auto& rop = view_as<FasterJtype> (instr);
							cpu.reg(REG_RA) = cpu.pc() + 4;
							cpu.registers().pc = rop.imm << PCALBITS;
						}); // JAL RA, pc+imm
				}
			}
			} // RV32I_JAL
			break;
		} // opcode
		// Rewritten compressed instructions
		else { // original.length() == 2
			const rv32c_instruction ci{instr};
			#define CI_CODE(x, y) ((x << 13) | (y))
			switch (ci.opcode()) {
			case CI_CODE(0b000, 0b01): // C.ADDI
				if (ci.CI.rd != 0) {
					// Destination register + expanded IMM
					instr.half[0] = ci.CI.rd | ((uint16_t)ci.CI.signed_imm() << 8);
					return rewritten_instruction<W>(
					[] (auto& cpu, auto instr) RVINSTR_ATTR {
						const auto reg    = instr.bytes[0];
						const int32_t imm = (int8_t)instr.bytes[1];
						cpu.reg(reg) += imm;
					});
				} break;
			case CI_CODE(0b010, 0b01): // C.LI
				if (ci.CI.rd != 0) {
					// Destination register + expanded IMM
					instr.half[0] = ci.CI.rd | ((uint16_t)ci.CI.signed_imm() << 8);
					return rewritten_instruction<W>(
					[] (auto& cpu, auto instr) RVINSTR_ATTR {
						const auto reg    = instr.bytes[0];
						const int32_t imm = (int8_t)instr.bytes[1];
						cpu.reg(reg) = imm;
					});
				} break;
			}
		}
		return decode(original);
	}

	template struct CPU<4>;
	template struct CPU<8>;
	template struct CPU<16>;
} // riscv
