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
		using mmio_cb_t = delegate<bool(Memory&, address_t, int, address_t)>;
		using page_fault_cb_t = delegate<Page&(Memory&, size_t)>;

		template <typename T>
		T read(address_t address);

		template <typename T>
		bool write(address_t address, T value);

		void memset(address_t dst, uint8_t value, size_t len);
		void memcpy(address_t dst, const void* src, size_t);
		void memcpy_out(void* dst, address_t src, size_t);

		auto copy_to_guest(address_t dst, const void* buf, size_t len)
		{
			this->memcpy(dst, buf, len);
			return dst + len;
		}

		address_t start_address() const noexcept { return this->m_start_address; }
		address_t stack_initial() const noexcept { return this->m_stack_address; }
		address_t elf_end_vaddr() const noexcept { return this->m_elf_end_vaddr; }

		bool is_writable(const address_t address) const noexcept;

		auto& machine() { return this->m_machine; }
		const auto& machine() const { return this->m_machine; }

		void reset();

		// memory traps
		static constexpr int TRAP_READ  = 0x0;
		static constexpr int TRAP_WRITE = 0x1000;
		static constexpr int TRAP_EXEC  = 0x2000;
#ifdef RISCV_DEBUG
		// NOTE: breaking traps will pause on the *next* instruction
		// NOTE: use print_and_pause() instead!
		void trap(address_t address, mmio_cb_t callback);
#endif

		// page handling
		size_t active_pages() const noexcept { return m_pages.size(); }
		size_t total_pages() const noexcept { return this->m_pages_total; }
		void set_pages_total(size_t new_max) noexcept { this->m_pages_total = new_max; }
		auto& pages() noexcept { return m_pages; }
		const Page& get_page(const address_t address) const noexcept;
		Page& create_page(const address_t address);

		void set_page_fault_handler(page_fault_cb_t h) { this->m_page_fault_handler = h; }
		static Page& default_page_fault(Memory&, const size_t page);

		Memory(Machine<W>&, std::vector<uint8_t>, bool protect_memory);
	private:
		inline auto& create_attr(const address_t address);
		inline void  set_page_attr(address_t, size_t len, PageAttributes);
		static inline uintptr_t page_number(const address_t address) {
			return address >> Page::SHIFT;
		}
		using Ehdr = typename Elf<W>::Ehdr;
		using Phdr = typename Elf<W>::Phdr;
		using Shdr = typename Elf<W>::Shdr;
		void binary_loader();
		void binary_load_ph(const Phdr*);
		void initial_paging();
		void protection_fault();

		Machine<W>& m_machine;

		address_t m_start_address = 0;
		address_t m_stack_address = 0;
		address_t m_elf_end_vaddr = 0;
		size_t    m_pages_total   = 256; // max physical memory usage

		std::unordered_map<address_t, Page> m_pages;
		page_fault_cb_t m_page_fault_handler = nullptr;

		std::vector<uint8_t> m_binary;
		bool m_protect_segments;

#ifdef RISCV_DEBUG
		// map of page-indexed trap functions
		// NOTE: uses page-numbers, not byte-addressing
		std::unordered_map<address_t, mmio_cb_t> m_callbacks;
#endif
		inline constexpr bool check_trap(address_t, int size, address_t value);
	};
#include "memory_inline.hpp"
}
