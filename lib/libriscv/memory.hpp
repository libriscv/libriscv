#pragma once
#include "common.hpp"
#include "elf.hpp"
#include "types.hpp"
#include "page.hpp"
#include "util/delegate.hpp"
#include <cassert>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
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
		T read(address_t address);

		template <typename T>
		void write(address_t address, T value);

		void memset(address_t dst, uint8_t value, size_t len);
		void memcpy(address_t dst, const void* src, size_t);
		void memcpy_out(void* dst, address_t src, size_t);

		address_t start_address() const noexcept { return this->m_start_address; }
		address_t stack_initial() const noexcept { return this->m_stack_address; }
		address_t elf_end_vaddr() const noexcept { return this->m_elf_end_vaddr; }

		bool is_writable(const address_t address) const noexcept;

		auto& machine() { return this->m_machine; }
		const auto& machine() const { return this->m_machine; }

		void reset();

		// call interface
		address_t resolve_address(const std::string& sym);


		// memory traps
		// NOTE: use print_and_pause() to immediately break!
		void trap(address_t page_addr, mmio_cb_t callback);

		// page handling
		size_t active_pages() const noexcept { return m_pages.size(); }
		size_t highest_active_pages() const noexcept { return m_pages_highest; }
		size_t total_pages() const noexcept { return this->m_pages_total; }
		void set_pages_total(size_t new_max) noexcept { this->m_pages_total = new_max; }
		auto& pages() noexcept { return m_pages; }
		const Page& get_page(const address_t address) const noexcept;
		const Page& get_pageno(const address_t npage) const noexcept;
		Page& create_page(const address_t npage);
		void  set_page_attr(address_t, size_t len, PageAttributes);
		// page creation & destruction
		Page& allocate_page(const size_t page);
		void  free_pages(address_t, size_t len);

		void set_page_fault_handler(page_fault_cb_t h) { this->m_page_fault_handler = h; }
		static Page& default_page_fault(Memory&, const size_t page);

		Memory(Machine<W>&, const std::vector<uint8_t>&, bool protect_memory);
	private:
		inline auto& create_attr(const address_t address);
		static inline uintptr_t page_number(const address_t address) {
			return address >> Page::SHIFT;
		}
		void initial_paging();
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
		size_t    m_pages_total   = 256; // max physical memory usage
		size_t    m_pages_highest = 0;

		const Page* m_current_rd_ptr  = nullptr;
		address_t   m_current_rd_page = -1;
		Page*     m_current_wr_ptr  = nullptr;
		address_t m_current_wr_page = -1;
		std::unordered_map<address_t, Page> m_pages;
		page_fault_cb_t m_page_fault_handler = nullptr;

		const std::vector<uint8_t>& m_binary;
		bool m_protect_segments;
	};
#include "memory_inline.hpp"
}
