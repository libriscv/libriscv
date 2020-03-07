#pragma once
#include "common.hpp"
#include "elf.hpp"
#include "types.hpp"
#include "page.hpp"
#include "util/delegate.hpp"
#include <cassert>
#include <cstring>
#include <EASTL/string.h>
#include <EASTL/string_map.h>
#include <EASTL/unordered_map.h>
#include <vector>

namespace riscv
{
	template<int W> struct Machine;

	template<int W>
	struct Memory
	{
		using address_t = address_type<W>;
		using isa_t     = isa_type<W>;
		using mmio_cb_t = Page::mmio_cb_t;
		using page_fault_cb_t = delegate<Page&(Memory&, size_t)>;

		template <typename T>
		T read(address_t src);

		template <typename T>
		void write(address_t dst, T value);

		void memset(address_t dst, uint8_t value, size_t len);
		void memcpy(address_t dst, const void* src, size_t);
		void memcpy_out(void* dst, address_t src, size_t);
		// gives a sequential view of the data at address, with the possibility
		// of optimizing away a copy if the data crosses no page-boundaries
		void memview(address_t addr, size_t len,
					delegate<void(const uint8_t*, size_t)> callback);
		// read a zero-terminated string directly from guests memory
		std::string memstring(address_t addr, size_t max_len = 1024);

		address_t start_address() const noexcept { return this->m_start_address; }
		address_t stack_initial() const noexcept { return this->m_stack_address; }
		address_t elf_end_vaddr() const noexcept { return this->m_elf_end_vaddr; }

		auto& machine() { return this->m_machine; }
		const auto& machine() const { return this->m_machine; }

		// call interface
		address_t resolve_address(const char* sym);
		void      set_exit_address(address_t new_exit);
		address_t exit_address() const noexcept;

		// page handling
		size_t pages_active() const noexcept { return m_pages.size(); }
		size_t pages_highest_active() const noexcept { return m_pages_highest; }
		size_t pages_total() const noexcept { return this->m_pages_total; }
		void set_pages_total(size_t new_max) noexcept { this->m_pages_total = new_max; }
		auto& pages() noexcept { return m_pages; }
		const Page& get_page(address_t) const noexcept;
		const Page& get_pageno(address_t npage) const noexcept;
		Page& create_page(address_t npage);
		void  set_page_attr(address_t, size_t len, PageAttributes);
		const PageAttributes& get_page_attr(address_t) const noexcept;
		// page creation & destruction
		Page& allocate_page(const size_t page);
		void  free_pages(address_t, size_t len);
		// page faults
		void set_page_fault_handler(page_fault_cb_t h) { this->m_page_fault_handler = h; }
		static Page& default_page_fault(Memory&, const size_t page);
		// NOTE: use print_and_pause() to immediately break!
		void trap(address_t page_addr, mmio_cb_t callback);

		const auto& binary() const noexcept { return m_binary; }
		void reset();
		// serializes all the machine state + a tiny header to @vec
		void serialize_to(std::vector<uint8_t>& vec);
		// returns the machine to a previously stored state
		void deserialize_from(const std::vector<uint8_t>&, const SerializedMachine<W>&);

		Memory(Machine<W>&, const std::vector<uint8_t>&, address_t max_mem);
	private:
		inline auto& create_attr(const address_t address);
		static inline uintptr_t page_number(const address_t address) {
			return address >> Page::SHIFT;
		}
		void initial_paging();
		void invalidate_page(address_t pageno, Page&);
		void protection_fault();
		// ELF stuff
		using Ehdr = typename Elf<W>::Ehdr;
		using Phdr = typename Elf<W>::Phdr;
		using Shdr = typename Elf<W>::Shdr;
		void binary_loader();
		void binary_load_ph(const Phdr*);
		template <typename T> T* elf_offset(intptr_t ofs) const {
			return (T*) &m_binary.at(ofs);
		}
		inline const auto* elf_header() const noexcept {
			return elf_offset<const Ehdr> (0);
		}
		const Shdr* section_by_name(const char* name) const;
		void relocate_section(const char* section_name, const char* symtab);
		const typename Elf<W>::Sym* resolve_symbol(const char* name);
		const auto* elf_sym_index(const Shdr* shdr, uint32_t symidx) const {
			assert(symidx < shdr->sh_size / sizeof(typename Elf<W>::Sym));
			auto* symtab = elf_offset<typename Elf<W>::Sym>(shdr->sh_offset);
			return &symtab[symidx];
		}

		Machine<W>& m_machine;

		address_t m_start_address = 0;
		address_t m_stack_address = 0;
		address_t m_elf_end_vaddr = 0;
		size_t    m_pages_total   = 0; // max memory usage
		size_t    m_pages_highest = 0; // max pages used

		const Page* m_current_rd_ptr  = nullptr;
		address_t   m_current_rd_page = -1;
		Page*     m_current_wr_ptr  = nullptr;
		address_t m_current_wr_page = -1;
		eastl::unordered_map<address_t, Page> m_pages;
		page_fault_cb_t m_page_fault_handler = nullptr;

		const std::vector<uint8_t>& m_binary;
		bool m_protect_segments;

		// lookup tree for ELF symbol names
		eastl::string_map<address_t> sym_lookup;
		address_t m_exit_address = 0;
	};
#include "memory_inline.hpp"
}
