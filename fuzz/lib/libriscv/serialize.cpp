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

		Registers<W> registers;
		uint64_t     counter;

		uint64_t start_address = 0;
		uint64_t stack_address = 0;
		uint64_t mmap_address  = 0;
		uint64_t heap_address  = 0;
		uint64_t exit_address  = 0;
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
			.n_pages  = (unsigned) memory.owned_pages_active(),
			.reg_size = sizeof(Registers<W>),
			.page_size = Page::size(),
			.attr_size = sizeof(PageAttributes),
			.reserved = 0,
			.cpu_offset = sizeof(SerializedMachine<W>),
			.mem_offset = sizeof(SerializedMachine<W>),

			.registers = cpu.registers(),
			.counter   = this->instruction_counter(),

			.start_address = memory.start_address(),
			.stack_address = memory.stack_initial(),
			.mmap_address  = memory.mmap_address(),
			.heap_address  = memory.heap_address(),
			.exit_address  = memory.exit_address(),
		};
		const auto* hptr = (const uint8_t*) &header;
		vec.insert(vec.end(), hptr, hptr + sizeof(header));
		this->cpu.serialize_to(vec);
		this->memory.serialize_to(vec);
	}
	template <int W>
	void CPU<W>::serialize_to(std::vector<uint8_t>& /* vec */)
	{
	}
	template <int W>
	void Memory<W>::serialize_to(std::vector<uint8_t>& vec)
	{
		const size_t est_page_bytes =
			this->m_pages.size() * (sizeof(SerializedPage) + Page::size());
		vec.reserve(vec.size() + est_page_bytes);

		for (const auto& it : this->m_pages)
		{
			const auto& page = it.second;
			assert(!page.attr.is_cow && "Should never have CoW pages stored");
			// we want to ignore shared/non-owned pages
			if (page.attr.non_owning) continue;
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
		this->m_counter = header.counter;
		cpu.deserialize_from(vec, header);
		memory.deserialize_from(vec, header);
		return 0;
	}
	template <int W>
	void CPU<W>::deserialize_from(const std::vector<uint8_t>& /* vec */,
					const SerializedMachine<W>& state)
	{
		// restore CPU registers and counters
		this->m_regs = state.registers;
		this->m_cache = {};
#ifdef RISCV_EXT_ATOMICS
		this->m_atomics = {};
#endif
		this->aligned_jump(this->pc());
	}
	template <int W>
	void Memory<W>::deserialize_from(const std::vector<uint8_t>& vec,
					const SerializedMachine<W>& state)
	{
		this->m_start_address = state.start_address;
		this->m_stack_address = state.stack_address;
		this->m_mmap_address  = state.mmap_address;
		this->m_heap_address  = state.heap_address;
		this->m_exit_address  = state.exit_address;

		[[maybe_unused]]
		const size_t page_bytes =
			state.n_pages * (sizeof(SerializedPage) + Page::size());
		assert(vec.size() >= state.mem_offset + page_bytes);
		// completely reset the paging system as
		// all pages will be completely replaced
		this->clear_all_pages();

		if (m_exec_pagedata != nullptr && m_exec_pagedata_size > 0)
		{
			// TODO: serialize the executable memory separately?
			// XXX: this only works if you restore to the same machine
			// XXX: Does not work with FASTSIM
#ifndef RISCV_FAST_SIMULATOR
			this->insert_non_owned_memory(
				m_exec_pagedata_base, m_exec_pagedata.get(), m_exec_pagedata_size, {
					.read = true, .write = false, .exec = true
				});
#endif
		}

		size_t off = state.mem_offset;
		for (size_t p = 0; p < state.n_pages; p++) {
			const auto& page = *(SerializedPage*) &vec[off];
			off += sizeof(SerializedPage);
			const auto& data = *(PageData*) &vec[off];
			// when we serialized non-owning pages, we lost the connection
			// so now we own the page data
			PageAttributes new_attr = page.attr;
			new_attr.non_owning = false;
			m_pages.emplace(std::piecewise_construct,
				std::forward_as_tuple(page.addr),
				std::forward_as_tuple(new_attr, data)
			);

			off += Page::size();
		}
		// page tables have been changed
		this->invalidate_reset_cache();
	}

	template struct Machine<4>;
	template struct Machine<8>;
	template struct CPU<4>;
	template struct CPU<8>;
	template struct Memory<4>;
	template struct Memory<8>;
}
