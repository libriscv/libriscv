#include "machine.hpp"
#include "decoder_cache.hpp"
#include "instruction_list.hpp"
#include "rv32i_instr.hpp"
#include "rvc.hpp"

#ifdef RISCV_FAST_SIMULATOR
#include "fastsim.hpp"
namespace riscv
{
	static constexpr size_t  QC_TRESHOLD = 8;
	static constexpr size_t  QC_MAX = 0xFFFF;
	static constexpr int32_t FS_JALI = 40;
	static constexpr size_t  FS_MAXI = 65534;

	template <int W>
	static bool fastsim_gucci_opcode(rv32i_instruction instr)
	{
		if (instr.is_long()) {
			if (UNLIKELY(instr.opcode() == RV32I_JALR)) return false;
			if (UNLIKELY(instr.opcode() == RV32I_JAL)) {
				return (std::abs(instr.Jtype.jump_offset()) < FS_JALI * 4);
			}
			return true;
		}
		const rv32c_instruction ci { instr };
		#define CI_CODE(x, y) ((x << 13) | (y))
		switch (ci.opcode()) {
		case CI_CODE(0b001, 0b01):
			if constexpr (W >= 8) return true; // C.ADDIW
			return false; // C.JAL 32-bit
		case CI_CODE(0b101, 0b01): // C.JMP
		case CI_CODE(0b110, 0b01): // C.BEQZ
		case CI_CODE(0b111, 0b01): // C.BNEZ
			return false;
		case CI_CODE(0b100, 0b10): { // VARIOUS
				const bool topbit = ci.whole & (1 << 12);
				if (!topbit && ci.CR.rd != 0 && ci.CR.rs2 == 0) {
					return false; // C.JR rd
				} else if (topbit && ci.CR.rd != 0 && ci.CR.rs2 == 0) {
					return false; // C.JALR ra, rd+0
				} // TODO: Handle C.EBREAK
				return true;
			}
		default:
			return true;
		}
	}
	template <int W>
	static void realize_qcvec(QCVec<W>& qcvec, size_t fsidx, uint8_t* exec_segment, DecoderData<W>* exec_decoder)
	{
		//printf("Fast sim idx %zu at 0x%lX -> 0x%lX, %zu good instructions\n",
		//	fsidx, (long)qcvec.base_pc, (long)qcvec.end_pc, qcvec.data.size());

		// Count distance to next branching instruction backwards
		// and fill in idxend for all entries along the way.
		unsigned idxend = qcvec.data.size();
		for (int i = (int)qcvec.data.size()-1; i >= 0; i--)
		{
			auto& cdata = qcvec.data[i];
			const auto opcode = cdata.original_opcode;
			if (opcode == RV32I_BRANCH || opcode == RV32I_SYSTEM
				|| opcode == RV32I_JAL || opcode == RV32I_JALR)
				idxend = i + 1;
			cdata.idxend = idxend;
			//printf("fs: %zu i: %d  idxend: %u\n", qc_lastidx, i, cdata.idxend);
		}

		auto pc = qcvec.base_pc;
		for (size_t i = 0; i < qcvec.data.size(); i++)
		{
			// Write the current FSim index into the instruction stream
			auto* half = (uint16_t*) &exec_segment[pc];
			half[0] = fsidx;
			// Set the fast simulator handler at current PC
			auto& qc_entry = exec_decoder[pc / DecoderCache<W>::DIVISOR];
			qc_entry.set(&CPU<W>::fast_simulator);
			// Advance to the next accelerated instruction
			if constexpr (compressed_enabled) {
				const auto& idata = qcvec.data[i];
				pc += rv32i_instruction{idata.instr}.length();
			} else pc += 4;
		}
#  ifdef RISCV_EXT_COMPRESSED
		// The outer simulator (once fast sim ends) will be
		// trying to calculate the instruction length based
		// on the index number written into the 16-bit
		// instruction. To compensate, we add the real
		// instructions length and subtract the "wrong" length.
		const bool will_be_long = (qc_lastidx & 0x3) == 0x3;
		qcvec.incrementor = rv32i_instruction{qcvec.data.front().instr}.length();
		qcvec.incrementor -= will_be_long ? 4 : 2;
#  endif
	}
}
#else
#define VERBOSE_FASTSIM  false
#endif // RISCV_FAST_SIMULATOR

namespace riscv
{
#ifdef RISCV_INSTR_CACHE
	template <int W>
	void Memory<W>::generate_decoder_cache(const MachineOptions<W>& options,
		address_t pbase, address_t addr, size_t len)
	{
		constexpr size_t PMASK = Page::size()-1;
		const size_t prelen  = addr - pbase;
		const size_t midlen  = len + prelen;
		const size_t plen = (midlen + PMASK) & ~PMASK;

		const size_t n_pages = plen / Page::size();
		auto* decoder_array = new DecoderCache<W> [n_pages];
		this->m_exec_decoder =
			decoder_array[0].get_base() - pbase / decoder_array->DIVISOR;
		// there could be an old cache from a machine reset
		delete[] this->m_decoder_cache;
		this->m_decoder_cache = &decoder_array[0];

		auto* exec_segment = this->get_exec_segment(pbase);
		assert(exec_segment != nullptr && "Must have set CPU execute segment");
		auto* exec_decoder = this->m_exec_decoder;

	#ifdef RISCV_BINARY_TRANSLATION
		std::string bintr_filename;
		// This can be improved somewhat, by fetching them on demand
		// instead of building a vector of the whole execute segment.
		std::vector<typename CPU<W>::instr_pair> ipairs;
		ipairs.reserve(len / 4);

	if constexpr (W != 16) {
		machine().cpu.load_translation(options, &bintr_filename);
	} // W != 16
	#endif

#ifdef RISCV_FAST_SIMULATOR
		size_t qc_lastidx = (options.fast_simulator) ? 0 : QC_MAX;
		size_t qc_instrcnt = 0;
		size_t qc_failure = 0;
		QCVec<W> qcvec;
		qcvec.base_pc = addr;
#endif
		// When compressed instructions are enabled, many decoder
		// entries are illegal because they between instructions.
		bool was_full_instruction = true;

		/* Generate all instruction pointers for executable code.
		   Cannot step outside of this area when pregen is enabled,
		   so it's fine to leave the boundries alone. */
		for (address_t dst = addr; dst < addr + len;)
		{
			auto& entry = exec_decoder[dst / DecoderCache<W>::DIVISOR];

#ifdef RISCV_BINARY_TRANSLATION
				if (machine().is_binary_translated()) {
					if (DecoderCache<W>::isset(entry)) {
						dst += 4;
	#ifdef RISCV_FAST_SIMULATOR
						qcvec.data.clear();
						qcvec.base_pc = dst;
	#endif
						continue;
					}
				} else if constexpr (W != 16) {
					// This may be a misaligned reference
					// XXX: Will this even work on ARM?
					auto& instref = *(rv32i_instruction*) &exec_segment[dst];
	#ifdef RISCV_DEBUG
					ipairs.emplace_back(entry.handler.handler, instref);
	#else
					ipairs.emplace_back(entry.handler, instref);
	#endif
				}
#endif

			// Load unaligned instruction from execute segment
			union Align32 {
				uint16_t data[2];
				operator uint32_t() {
					return data[0] | uint32_t(data[1]) << 16;
				}
			};
			rv32i_instruction instruction { *(Align32*) &exec_segment[dst] };
			const auto original = instruction;

			// Insert decoded instruction into decoder cache
			Instruction<W> decoded;
			bool is_rewritten = false;
			if (!was_full_instruction) {
				// An illegal instruction
				decoded = machine().cpu.decode(rv32i_instruction{0});
			} else if constexpr (debugging_enabled) {
				// When debugging we will want to see the original encoding, as
				// well as needing more trust in the decoding.
				decoded = machine().cpu.decode(instruction);
			} else {
				bool try_fuse = options.instruction_fusing;
				// Fast simulator gets confusing instruction logging with instr rewrites
				if constexpr (decoder_rewriter_enabled && !VERBOSE_FASTSIM) {
					// Improve many instruction handlers by rewriting instructions
					decoded = machine().cpu.decode_rewrite(dst, instruction);
					// Write the instruction back to execute segment using
					// the *original* instructions length, if it changed.
					is_rewritten = original.whole != instruction.whole;
					if (is_rewritten) {
						assert(original.length() == instruction.length());
						std::memcpy((void*)&exec_segment[dst], &instruction, original.length());
						try_fuse = false;
					}
				} else {
					decoded = machine().cpu.decode(instruction);
				}
				if (!is_rewritten && try_fuse) {
					// TODO: Instruction fusing here
				}
			}
			DecoderCache<W>::convert(decoded, entry);

#ifdef RISCV_FAST_SIMULATOR
			if (LIKELY(qc_lastidx < QC_MAX) && was_full_instruction) {
				const rv32i_instruction qc_instr = instruction;
				// Store original opcode in the QCData struct so that
				// we can go back and put end indices correctly. In theory
				// we could just overwrite idxend to save a member, but
				// it might be useful to know the original opcode in case
				// the opcode rewriter has some issue.
				const uint8_t original_opcode = original.opcode();
				// We will verify the original instruction only,
				// as it should still have the same semantics.
				if (fastsim_gucci_opcode<W>(original) && qcvec.data.size() < FS_MAXI) {
					qcvec.data.push_back({decoded.handler, qc_instr.whole, 0, original_opcode, 0});
				} else {
					if (qcvec.data.size()+1 >= QC_TRESHOLD) {
						qcvec.data.push_back({decoded.handler, qc_instr.whole, 0, original_opcode, 0});
						qcvec.end_pc = dst + (compressed_enabled ? qc_instr.length() : 4);

						realize_qcvec<W>(qcvec, qc_lastidx, exec_segment, exec_decoder);

						// Add the instruction data array to the CPU
						qc_lastidx ++;
						qc_instrcnt += qcvec.data.size();
						machine().cpu.add_qc(std::move(qcvec));
					} else {
						qc_failure ++;
					}
					qcvec.data.clear();
					qcvec.base_pc = dst + (compressed_enabled ? instruction.length() : 4);
				}
			} // options.fast_simulator
#endif
			// Increment PC after everything
			if constexpr (compressed_enabled) {
				// With compressed we always step forward 2 bytes at a time
				dst += 2;
				if (was_full_instruction) {
					// For it to be a full instruction again,
					// the length needs to match.
					was_full_instruction = (original.length() == 2);
				} else {
					// If it wasn't a full instruction last time, it
					// will for sure be one now.
					was_full_instruction = true;
				}
			} else
				dst += 4;
		}

#ifdef RISCV_FAST_SIMULATOR
		machine().cpu.finish_qc();
		if (options.verbose_loader) {
			printf("Fast sim conversion blocks: %zu  Instructions: %zu\n", qc_lastidx, qc_instrcnt);
			printf("Fast sim conversion failure: %zu\n", qc_failure);
		}
#endif

#ifdef RISCV_BINARY_TRANSLATION
		/* We do not support binary translation for RV128I */
		if constexpr (W != 16) {
			if (!machine().is_binary_translated()) {
				machine().cpu.try_translate(options, bintr_filename, addr, ipairs);
			}
		} // W != 16
#endif
	}
#endif // RISCV_INSTR_CACHE

	template struct Memory<4>;
	template struct Memory<8>;
	template struct Memory<16>;
} // riscv
