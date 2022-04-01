#include "machine.hpp"
#include "decoder_cache.hpp"
#include "instruction_list.hpp"
#include "rv32i_instr.hpp"
#include "instr_helpers.hpp"

namespace riscv
{
	static const unsigned PCAL= compressed_enabled ? 2 : 4;

	union FasterBtype {
		struct {
			uint8_t opcode : 3;
			uint8_t rs2    : 5;
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
			uint8_t opcode;
			uint8_t rs1;
			int16_t imm;
		};
		uint32_t whole;
		int32_t signed_imm() const noexcept {
			return imm;
		}
	};
	static_assert(sizeof(ZeroBtype) == 4, "is a 4-byte instruction");

	template <int W> RVPRINTR_ATTR
	int rewritten_instr_printer(char* buffer, size_t len, const CPU<W>&, rv32i_instruction) {
		return snprintf(buffer, len, "Rewritten instruction");
	}

	template <typename T>
	T& view_as(rv32i_instruction& i) {
		static_assert(sizeof(T) == sizeof(i), "Must be same size as instruction!");
		return *(T*) &i;
	}

	template <int W> inline
	Instruction<W> rewritten_instruction(instruction_handler<W> func) {
		return {func, rewritten_instr_printer};
	}

	template <int W>
	Instruction<W> CPU<W>::decode_rewrite(address_t pc, rv32i_instruction& instr)
	{
		using sign_type = typename std::make_signed<address_t>::type;
		const auto original = instr;
		(void) pc;

		// Rewrite all B-type instructions to become saner
		// NOTE: Set aside the first 2 bits to preserve the instruction length
		switch (original.opcode()) {
		case RV32I_BRANCH: {
			// Ignore unaligned branches. This will allow us to
			// jump unaligned with all branch instructions.
			const auto bdest = pc + original.Btype.signed_imm() - 4;
			if ((bdest % PCAL) != 0) break;
			// We verify if the jump is within the execute segment
			// and if not, we fallback to the regular branch decoding
		#ifdef RISCV_INBOUND_JUMPS_ONLY
			if (UNLIKELY(bdest < m_exec_begin || bdest >= m_exec_end)) {
				break;
			}
		#endif

			if (original.Btype.rs2 == 0) {
				ZeroBtype rewritten;
				rewritten.opcode = original.Btype.opcode;
				rewritten.rs1 = original.Btype.rs1;
				rewritten.imm = original.Btype.signed_imm() - 4;
				assert(original.Btype.rs1 == rewritten.rs1);
				assert(original.Btype.signed_imm()-4 == rewritten.signed_imm());
				assert(original.length() == rv32i_instruction{rewritten.whole}.length());
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
			rewritten.opcode = original.Btype.opcode;
			rewritten.rs2 = original.Btype.rs2;
			rewritten.rs1 = original.Btype.rs1;
			rewritten.imm = original.Btype.signed_imm() - 4;
			assert(original.Btype.signed_imm()-4 == rewritten.signed_imm());
			assert(original.Btype.rs1 == rewritten.rs1);
			assert(original.Btype.rs2 == rewritten.rs2);
			assert(original.length() == rv32i_instruction{rewritten.whole}.length());
			instr.whole = rewritten.whole;

			switch (original.Btype.funct3) {
			case 0x0: // BRANCH_EQ
				return rewritten_instruction<W>(
					[] (auto& cpu, auto instr) RVINSTR_ATTR {
						const auto& rop = view_as<FasterBtype> (instr);
						if (cpu.reg(rop.rs1) == cpu.reg(rop.rs2)) {
							cpu.registers().pc += rop.signed_imm();
						}
					}); // BEQ
			case 0x1: // BRANCH_NE
				return rewritten_instruction<W>(
					[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
						const auto& rop = view_as<FasterBtype> (instr);
						if (cpu.reg(rop.rs1) != cpu.reg(rop.rs2)) {
							cpu.registers().pc += rop.signed_imm();
						}
					}); // BNE
			case 0x4: // BRANCH_LT
				return rewritten_instruction<W>(
					[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
						const auto& rop = view_as<FasterBtype> (instr);
						if (sign_type(cpu.reg(rop.rs1)) < sign_type(cpu.reg(rop.rs2))) {
							cpu.registers().pc += rop.signed_imm();
						}
					}); // BLT
			case 0x5: // BRANCH_GE
				return rewritten_instruction<W>(
					[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
						const auto& rop = view_as<FasterBtype> (instr);
						if (sign_type(cpu.reg(rop.rs1)) >= sign_type(cpu.reg(rop.rs2))) {
							cpu.registers().pc += rop.signed_imm();
						}
					}); // BGE
			case 0x6: // BRANCH_LTU
				return rewritten_instruction<W>(
					[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
						const auto& rop = view_as<FasterBtype> (instr);
						if (cpu.reg(rop.rs1) < cpu.reg(rop.rs2)) {
							cpu.registers().pc += rop.signed_imm();
						}
					}); // BLTU
			case 0x7: // BRANCH_GEU
				return rewritten_instruction<W>(
					[] (auto& cpu, rv32i_instruction instr) RVINSTR_ATTR {
						const auto& rop = view_as<FasterBtype> (instr);
						if (cpu.reg(rop.rs1) >= cpu.reg(rop.rs2)) {
							cpu.registers().pc += rop.signed_imm();
						}
					}); // BGEU
			default:
				// Restore original for invalid BRANCH instructions
				instr = original;
			} // BRANCH type
			} // RV32I_BRANCH
		} // opcode
		return decode(original);
	}

	template struct CPU<4>;
	template struct CPU<8>;
	template struct CPU<16>;
} // riscv
