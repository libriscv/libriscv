#include "memory.hpp"
#include "machine.hpp"
#include "decoder_cache.hpp"
#include <stdexcept>

#include "rv32i_instr.hpp"

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
		std::vector<typename CPU<W>::instr_pair> ipairs;
		ipairs.reserve(len / 4);

		auto* exec_offset = machine().cpu.exec_seg_data();
		assert(exec_offset && "Must have set CPU execute segment");

		/* Generate all instruction pointers for executable code.
		   Cannot step outside of this area when pregen is enabled,
		   so it's fine to leave the boundries alone. */
		for (address_t dst = addr; dst < addr + len;)
		{
			auto& entry = m_exec_decoder[dst / DecoderCache<W>::DIVISOR];

			auto& instruction = *(rv32i_instruction*) &exec_offset[dst];
			if (binary_translation_enabled || options.instruction_fusing) {
				ipairs.emplace_back(entry.handler, instruction);
			}

			DecoderCache<W>::convert(machine().cpu.decode(instruction), entry);
			// We do not cache 2-byte mid-aligned instructions
			dst += 4;
		}

#ifdef RISCV_BINARY_TRANSLATION
		/* We do not support binary translation for RV128I */
		if constexpr (W != 16) {
			machine().cpu.try_translate(options, addr, ipairs);
		}
#endif

		/* We do not support fusing for RV128I */
		if (options.instruction_fusing && W != 16) {
			for (size_t n = 0; n < ipairs.size()-1; n++)
			{
				if (machine().cpu.try_fuse(ipairs[n+0], ipairs[n+1]))
					n += 1;
			}
		}

#else
		// zero the whole thing
		std::memset(decoder_array, 0, n_pages * sizeof(decoder_array[0]));
#endif
		(void) options;
	}
#endif

	template struct Memory<4>;
	template struct Memory<8>;
	template struct Memory<16>;
}
