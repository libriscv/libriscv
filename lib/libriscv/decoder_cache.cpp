#include "machine.hpp"
#include "decoder_cache.hpp"
#include "instruction_list.hpp"
#include "rv32i_instr.hpp"
#include "rvc.hpp"

#ifdef RISCV_FAST_SIMULATOR
namespace riscv
{
	template <int W>
	static void realize_fastsim(address_type<W> base_pc, size_t count,
		DecoderData<W>* exec_decoder)
	{
		// Count distance to next branching instruction backwards
		// and fill in idxend for all entries along the way.
		unsigned idxend = 0;
		for (int i = (int)count-1; i >= 0; i--)
		{
			// Set the fast simulator handler at current PC
			const auto pc = base_pc + i * 4;
			auto& entry = exec_decoder[pc / DecoderCache<W>::DIVISOR];

			// All opcodes that can modify PC
			const auto opcode = entry.original_opcode;
			if (opcode == RV32I_BRANCH || opcode == RV32I_SYSTEM
				|| opcode == RV32I_JAL || opcode == RV32I_JALR)
				idxend = 0;
			idxend ++;
			entry.idxend = idxend;
		}
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
		std::vector<TransInstr<W>> ipairs;
		ipairs.reserve(len / 4);

	if constexpr (W != 16) {
		machine().cpu.load_translation(options, &bintr_filename);
	} // W != 16
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

			// Load unaligned instruction from execute segment
			union Align32 {
				uint16_t data[2];
				operator uint32_t() {
					return data[0] | uint32_t(data[1]) << 16;
				}
			};
			const rv32i_instruction instruction { *(Align32*) &exec_segment[dst] };
			rv32i_instruction rewritten = instruction;

#ifdef RISCV_BINARY_TRANSLATION
			if (machine().is_binary_translated()) {
				if (DecoderCache<W>::isset(entry)) {
					#ifdef RISCV_FAST_SIMULATOR
					// XXX: With fastsim we could pretend the original opcode
					// is a JAL here, which would break the fastsim loop
					entry.original_opcode = RV32I_JAL;
					#endif
					dst += 4;
					continue;
				}
			} else if constexpr (W != 16) {
#  ifdef RISCV_DEBUG
				ipairs.push_back({entry.handler.handler, instruction.whole});
#  else
				ipairs.push_back({entry.handler, instruction.whole});
#  endif
			}
#endif // RISCV_BINARY_TRANSLATION

#ifdef RISCV_FAST_SIMULATOR
			// Help the fastsim determine the real opcodes
			entry.original_opcode = instruction.opcode();
#endif

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
				if constexpr (decoder_rewriter_enabled) {
					// Improve many instruction handlers by rewriting instructions
					decoded = machine().cpu.decode_rewrite(dst, rewritten);
					// Write the instruction back to execute segment using
					// the *original* instructions length, if it changed.
					// But only if we do not need it later, eg. binary translation.
					is_rewritten = rewritten.whole != instruction.whole;
					if (is_rewritten) {
						assert(rewritten.length() == instruction.length());
						std::memcpy((void*)&exec_segment[dst], &rewritten, instruction.length());
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
			// Cache the (modified) instruction bits
			entry.instr = rewritten.whole;
#endif

			// Increment PC after everything
			if constexpr (compressed_enabled) {
				// With compressed we always step forward 2 bytes at a time
				dst += 2;
				if (was_full_instruction) {
					// For it to be a full instruction again,
					// the length needs to match.
					was_full_instruction = (instruction.length() == 2);
				} else {
					// If it wasn't a full instruction last time, it
					// will for sure be one now.
					was_full_instruction = true;
				}
			} else
				dst += 4;
		}

#ifdef RISCV_FAST_SIMULATOR
		realize_fastsim<W>(addr, len / 4, exec_decoder);
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
