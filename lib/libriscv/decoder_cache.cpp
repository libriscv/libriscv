#include "memory.hpp"
#include "machine.hpp"
#include "decoder_cache.hpp"
#include <stdexcept>

#include "rv32i_instr.hpp"

namespace riscv
{

#ifdef RISCV_INSTR_CACHE
	template <int W>
	void Memory<W>::generate_decoder_cache(address_t addr, size_t len)
	{
		constexpr address_t PMASK = Page::size()-1;
		const address_t pbase = addr & ~PMASK;
		const size_t prelen  = addr - pbase;
		const size_t midlen  = len + prelen;
		const size_t plen =
			(PMASK & midlen) ? ((midlen + Page::size()) & ~PMASK) : midlen;

		const size_t n_pages = plen / Page::size();
		auto* decoder_array = new DecoderCache<Page::SIZE> [n_pages];
#ifdef RISCV_EXEC_SEGMENT_IS_CONSTANT
		this->m_exec_decoder =
			decoder_array[0].template get_base<W>() - pbase / DecoderCache<Page::SIZE>::DIVISOR;
#endif
		size_t dcindex = 0;
		while (len > 0)
		{
			const size_t size = std::min(Page::size(), len);
			const size_t pageno = page_number(addr);
			// find page itself
			auto it = m_pages.find(pageno);
			if (it != m_pages.end()) {
				auto& page = it->second;
				if (page.attr.exec) {
					assert(page.decoder_cache() == nullptr);
					// assign slice
					auto* cache = &decoder_array[dcindex];
#ifdef RISCV_INSTR_CACHE_PER_PAGE
					page.m_decoder_cache.reset(cache);
					// only the first page owns the whole range
					page.attr.decoder_non_owned = (dcindex != 0);
#endif
					dcindex++;

					// generate instruction handler pointers for machine code
					for (address_t dst = addr; dst < addr + size;)
					{
						const address_t offset = dst & (Page::size()-1);
						rv32i_instruction instruction;
						instruction.whole = *(uint32_t*) (page.data() + offset);

						cache->template get<W> (offset / cache->DIVISOR) =
							machine().cpu.decode(instruction).handler;

						dst += instruction.length();
					}
				}
			}

			addr += size;
			len  -= size;
		}
	}
#endif

	template struct Memory<4>;
	template struct Memory<8>;
}
