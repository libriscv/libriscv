#pragma once
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

		Memory(Machine<W>&, std::vector<uint8_t>);

		template <typename T>
		T read(address_t address);

		template <typename T>
		bool write(address_t address, T value);

		auto  memset(address_t dst, uint8_t value, size_t len);
		auto  memcpy(address_t dst, const uint8_t* src, size_t);
		auto* memcpy_out(uint8_t* dst, address_t src, size_t);

		address_t start_address() const noexcept { return this->m_start_address; }
		address_t stack_address() const noexcept { return this->m_stack_address; }

		bool is_writable(const address_t address) const noexcept;

		auto& machine() { return this->m_machine; }
		const auto& machine() const { return this->m_machine; }

		void reset();
		void trap(address_t address, mmio_cb_t callback) {
			this->m_callbacks[address] = callback;
		}
		void set_traps_enabled(bool en) noexcept { this->m_traps_enabled = en; }

		// page handling
		size_t active_pages() const noexcept { return m_pages.size(); }
		size_t pages_total() const noexcept { return this->m_pages_total; }
		void set_pages_total(size_t new_max) noexcept { this->m_pages_total = new_max; }
		auto& pages() noexcept { return m_pages; }
		const Page& get_page(const address_t address) const noexcept;
		Page& create_page(const address_t address);

		void set_page_fault_handler(page_fault_cb_t h) { this->m_page_fault_handler = h; }
		static Page& default_page_fault(Memory&, const size_t page);
	private:
		inline auto& create_attr(const address_t address);
		inline void  set_page_attr(address_t, size_t len, PageAttributes);
		static inline uintptr_t page_number(const address_t address) {
			return address >> Page::SHIFT;
		}
		void binary_loader();
		void protection_fault();

		Machine<W>& m_machine;

		address_t m_start_address = 0;
		address_t m_stack_address = 0;
		size_t    m_pages_total   = 128; // max physical memory usage

		std::unordered_map<address_t, Page> m_pages;
		page_fault_cb_t m_page_fault_handler = nullptr;

		std::vector<uint8_t> m_binary;

		// map of page-indexed trap functions
		// NOTE: uses page-numbers, not byte-addressing
		std::unordered_map<address_t, mmio_cb_t> m_callbacks;
		bool m_traps_enabled = false; // a small optimization

		bool check_trap(address_t address, int size, address_t value) {
			if (this->m_traps_enabled == false) return true;
			auto it = m_callbacks.find(address);
			if (it == m_callbacks.end()) return true;
			// do the thing
			return it->second(*this, address, size, value);
		}
	};
#include "memory_inline.hpp"
}
