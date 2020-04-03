#include <libriscv/machine.hpp>

namespace riscv
{
	static const uint64_t MAGiC_V4LUE = 0x9c36ab9301aed873;
	template <int W>
	struct SerializedMachine
	{
		uint64_t magic;
		uint32_t n_pages;
		uint16_t reg_size;
		uint16_t page_size;
		uint16_t attr_size;
		uint16_t reserved;
		uint16_t cpu_offset;
		uint16_t mem_offset;

		Registers<W> registers[0];
	};
	struct SerializedPage
	{
		uint64_t addr;
		PageAttributes attr;
	};

	template <int W>
	void Machine<W>::serialize_to(std::vector<uint8_t>& vec)
	{
		const SerializedMachine<W> header {
			.magic    = MAGiC_V4LUE,
			.n_pages  = (unsigned) memory.pages_active(),
			.reg_size = sizeof(Registers<W>),
			.page_size = Page::size(),
			.attr_size = sizeof(PageAttributes),
			.reserved = 0,
			.cpu_offset = sizeof(SerializedMachine<W>),
			.mem_offset = sizeof(SerializedMachine<W>) + sizeof(Registers<W>),
		};
		const auto* hptr = (const uint8_t*) &header;
		vec.insert(vec.end(), hptr, hptr + sizeof(header));
		this->cpu.serialize_to(vec);
		this->memory.serialize_to(vec);
	}
	template <int W>
	void CPU<W>::serialize_to(std::vector<uint8_t>& vec)
	{
		auto* rptr = (const uint8_t*) &this->m_regs;
		vec.insert(vec.end(), rptr, rptr + sizeof(Registers<W>));
	}
	template <int W>
	void Memory<W>::serialize_to(std::vector<uint8_t>& vec)
	{
		const size_t page_bytes =
			this->m_pages.size() * (sizeof(SerializedPage) + Page::size());
		vec.reserve(vec.size() + page_bytes);

		for (const auto it : this->m_pages)
		{
			const auto& page = it.second;
			assert(page.attr.is_cow == false);
			const SerializedPage spage {
				.addr = it.first,
				.attr = page.attr
			};
			auto* sptr = (const uint8_t*) &spage;
			vec.insert(vec.end(), sptr, sptr + sizeof(SerializedPage));
			// page data
			auto* pptr = page.data();
			vec.insert(vec.end(), pptr, pptr + Page::size());
		}
	}

	template <int W>
	int Machine<W>::deserialize_from(const std::vector<uint8_t>& vec)
	{
		if (vec.size() < sizeof(SerializedMachine<W>)) {
			return -1;
		}
		const auto& header = *(const SerializedMachine<W>*) vec.data();
		if (header.magic != MAGiC_V4LUE)
			return -1;
		if (header.reg_size != sizeof(Registers<W>))
			return -2;
		if (header.page_size != Page::size())
			return -3;
		if (header.attr_size != sizeof(PageAttributes))
			return -4;
		cpu.deserialize_from(vec, header);
		memory.deserialize_from(vec, header);
		return 0;
	}
	template <int W>
	void CPU<W>::deserialize_from(const std::vector<uint8_t>& vec,
					const SerializedMachine<W>& state)
	{
		assert(vec.size() >= state.cpu_offset + sizeof(Registers<W>));
		// restore CPU registers and counters
		this->m_regs = *(const Registers<W>*) &vec[state.cpu_offset];
		this->m_atomics = {};
		// reset the instruction page pointer
		this->m_current_page = {};
#ifdef RISCV_PAGE_CACHE
		// reset the page cache
		this->m_page_cache = {};
		this->m_cache_iterator = 0;
#endif
	}
	template <int W>
	void Memory<W>::deserialize_from(const std::vector<uint8_t>& vec,
					const SerializedMachine<W>& state)
	{
		[[maybe_unused]]
		const size_t page_bytes =
			state.n_pages * (sizeof(SerializedPage) + Page::size());
		assert(vec.size() >= state.mem_offset + page_bytes);
		// completely reset the paging system as
		// all pages will be completely replaced
		this->m_pages.clear();
		this->m_current_rd_page = -1;
		this->m_current_rd_ptr  = nullptr;
		this->m_current_wr_page = -1;
		this->m_current_wr_ptr  = nullptr;

		size_t off = state.mem_offset;
		for (size_t p = 0; p < state.n_pages; p++) {
			const auto& page = *(SerializedPage*) &vec[off];
			off += sizeof(SerializedPage);
			const auto& data = *(PageData*) &vec[off];
			m_pages.emplace(page.addr, Page{page.attr, data, nullptr});
			off += Page::size();
		}
	}

	template struct Machine<4>;
	template struct CPU<4>;
	template struct Memory<4>;
}
