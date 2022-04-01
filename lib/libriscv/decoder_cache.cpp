#include "machine.hpp"
#include "decoder_cache.hpp"
#include "instruction_list.hpp"
#include "rv32i_instr.hpp"
#include "rvc.hpp"

#ifdef RISCV_FAST_SIMULATOR
#include "fastsim.hpp"
#ifdef RISCV_USE_RH_HASH
#include <robin_hood.h>
template <typename T>
using qc_unordered_set = robin_hood::unordered_flat_set<T>;
#else
#include <unordered_set>
template <typename T>
using qc_unordered_set = std::unordered_set<T>;
#endif

namespace riscv
{
	static const qc_unordered_set<uint8_t> fsim_jumpy_insn
	{
		RV32I_BRANCH,
		RV32I_JALR,
		RV32I_JAL,
		RV32I_SYSTEM,
	};
	static constexpr size_t QC_TRESHOLD = 8;
	static constexpr size_t QC_MAX = 0xFFFF;
	static constexpr size_t FS_MAXI = 4096;

	template <int W>
	static bool fastsim_gucci_opcode(rv32i_instruction instr) {
		if (instr.is_long())
			return fsim_jumpy_insn.count(instr.opcode()) == 0;
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
}
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
		const size_t plen =
			(PMASK & midlen) ? ((midlen + Page::size()) & ~PMASK) : midlen;

		const size_t n_pages = plen / Page::size();
		auto* decoder_array = new DecoderCache<W> [n_pages];
		this->m_exec_decoder =
			decoder_array[0].get_base() - pbase / decoder_array->DIVISOR;
		// there could be an old cache from a machine reset
		delete[] this->m_decoder_cache;
		this->m_decoder_cache = &decoder_array[0];

#ifdef RISCV_INSTR_CACHE_PREGEN
		auto* exec_offset = machine().cpu.exec_seg_data();
		assert(exec_offset && "Must have set CPU execute segment");
		auto* exec_decoder = this->m_exec_decoder;

	#ifdef RISCV_BINARY_TRANSLATION
		std::string bintr_filename;
	if constexpr (W != 16) {
		int load_result = machine().cpu.load_translation(options, &bintr_filename);
		// If we loaded a cached translated program, and fusing is
		// disabled, then we can fast-path the decoder cache
		if (load_result == 0 && !options.instruction_fusing) {
			/* Generate all instruction pointers for executable code.
			   Cannot step outside of this area when pregen is enabled,
			   so it's fine to leave the boundries alone. */
			for (address_t dst = addr; dst < addr + len;)
			{
				auto& entry = m_exec_decoder[dst / DecoderCache<W>::DIVISOR];

				auto& instruction = *(rv32i_instruction*) &exec_offset[dst];
				if (!DecoderCache<W>::isset(entry)) {
					DecoderCache<W>::convert(machine().cpu.decode(instruction), entry);
				}
				dst += (compressed_enabled) ? 2 : 4;
			}
			return;
		} // Success, not fusing
	} // W != 16
	#endif

		std::vector<typename CPU<W>::instr_pair> ipairs;
		if (binary_translation_enabled || options.instruction_fusing) {
			ipairs.reserve(len / 4);
		}

#ifdef RISCV_FAST_SIMULATOR
		size_t qc_lastidx = (options.fast_simulator) ? 0 : QC_MAX;
		size_t qc_failure = 0;
		bool fs_skip = false;
		QCVec<W> qcvec;
		qcvec.base_pc = addr;
		//static_assert(!compressed_enabled, "C-extension with fast simulator is under construction");
#endif

		/* Generate all instruction pointers for executable code.
		   Cannot step outside of this area when pregen is enabled,
		   so it's fine to leave the boundries alone. */
		for (address_t dst = addr; dst < addr + len;)
		{
			auto& entry = exec_decoder[dst / DecoderCache<W>::DIVISOR];

			if (binary_translation_enabled || options.instruction_fusing) {
				// This may be a misaligned reference
				// XXX: Will this even work on ARM?
				auto& instref = *(rv32i_instruction*) &exec_offset[dst];
#ifdef RISCV_DEBUG
				ipairs.emplace_back(entry.handler.handler, instref);
#else
				ipairs.emplace_back(entry.handler, instref);
#endif
			}

			// Load unaligned instruction from execute segment
			union Align32 {
				uint16_t data[2];
				operator uint32_t() {
					return data[0] | uint32_t(data[1]) << 16;
				}
			};
			rv32i_instruction instruction { *(Align32*) &exec_offset[dst] };

			// Insert decoded instruction in decoder cache
			auto decoded = machine().cpu.decode(instruction);
			DecoderCache<W>::convert(decoded, entry);

#ifdef RISCV_FAST_SIMULATOR
			if (LIKELY(qc_lastidx < QC_MAX) && !fs_skip) {
				const rv32i_instruction qc_instr = instruction;
				if (fastsim_gucci_opcode<W>(qc_instr) && qcvec.data.size() < FS_MAXI) {
					qcvec.data.push_back({qc_instr.whole, decoded.handler});
				} else {
					if (qcvec.data.size()+1 >= QC_TRESHOLD) {
						qcvec.data.push_back({qc_instr.whole, decoded.handler});
						qcvec.end_pc = dst + qc_instr.length();
						//printf("Fast simulator at 0x%lX, %zu good instructions\n",
						//	(long)qcvec.base_pc, qcvec.data.size());
						auto pc = qcvec.base_pc;
						const size_t max = compressed_enabled ? 1 : qcvec.data.size();
						for (size_t i = 0; i < max; i++)
						{
							// Write the resulting index into the instruction stream
							auto* half = (uint16_t*) &exec_offset[pc];
							half[0] = qc_lastidx;
							// Set the fast simulator handler at current PC
							auto& qc_entry = exec_decoder[pc / DecoderCache<W>::DIVISOR];
							qc_entry.set(&CPU<W>::fast_simulator);
							// Advance to the next accelerated instruction
							if constexpr (compressed_enabled) {
								const auto& idata = qcvec.data[i];
								pc += rv32i_instruction{idata.instr}.length();
							} else pc += 4;
						}
						// The outer simulator (once fast sim ends) will be
						// trying to calculate the instruction length based
						// on the index number written into the 16-bit
						// instruction. To compensate, we add the real
						// instructions length and subtract the "wrong" length.
						const bool will_be_long = (qc_lastidx & 0x3) == 0x3;
						qcvec.incrementor = rv32i_instruction{qcvec.data.front().instr}.length();
						qcvec.incrementor -= will_be_long ? 4 : 2;
						// Add the instruction data array to the CPU
						machine().cpu.add_qc(qcvec);
						qc_lastidx ++;
					} else {
						qc_failure ++;
					}
					qcvec.data.clear();
					qcvec.base_pc = dst + instruction.length();
				}
			} // options.fast_simulator
#endif
			// Increment PC after everything
			if constexpr (compressed_enabled) {
				// With compressed we always step forward 2 bytes at a time
				dst += 2;
#ifdef RISCV_FAST_SIMULATOR
				if (fs_skip) {
					fs_skip = false;
				} else if (instruction.length() == 4) {
					// We advanced only 2 but the real instruction was 4 bytes
					// Remember the instruction and skip one fast sim iteration
					fs_skip = true;
				}
#endif
			} else
				dst += 4;
		}

#ifdef RISCV_FAST_SIMULATOR
		machine().cpu.finish_qc();
		if (options.verbose_loader) {
			printf("Fast sim conversion blocks: %zu\n", qc_lastidx);
			printf("Fast sim conversion failure: %zu\n", qc_failure);
		}
#endif

		/* We do not support binary translation for RV128I */
		/* We do not support fusing for RV128I */
		if constexpr (W != 16) {

#ifdef RISCV_BINARY_TRANSLATION
		if (!machine().is_binary_translated()) {
			machine().cpu.try_translate(options, bintr_filename, addr, ipairs);
		}
#endif
		if (options.instruction_fusing) {
			for (size_t n = 0; n < ipairs.size()-1; n++)
			{
				if (machine().cpu.try_fuse(ipairs[n+0], ipairs[n+1]))
					n += 1;
			}
		}
	} // W != 16
#else
		// Default-initialize the whole thing
		for (size_t p = 0; p < n_pages; p++)
			decoder_array[p] = {};
#endif
		(void) options;
	}
#endif

	template struct Memory<4>;
	template struct Memory<8>;
	template struct Memory<16>;
}
