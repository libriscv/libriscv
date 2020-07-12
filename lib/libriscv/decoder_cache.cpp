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
					page.create_decoder_cache();
					auto* cache = page.decoder_cache();

					// generate instruction handler pointers for machine code
					for (address_t dst = addr; dst < addr + size;)
					{
						const address_t offset = dst & (Page::size()-1);
						rv32i_instruction instruction;
						instruction.whole = *(uint32_t*) (page.data() + offset);

						cache->cache32[offset / cache->DIVISOR] =
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
}
