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

		Memory(Machine<W>&, std::vector<uint8_t>);

		template <int SIZE>
		inline auto read(address_t address);

		template <int SIZE, typename T>
		inline void write(address_t address, T value);

		inline auto  memset(address_t dst, uint8_t value, size_t len);
		inline auto  memcpy(address_t dst, const uint8_t* src, size_t);
		inline auto* memcpy_out(uint8_t* dst, address_t src, size_t);

		address_t start_address() const noexcept { return this->m_start_address; }
		address_t stack_address() const noexcept { return this->m_stack_address; }

		size_t active_pages() const noexcept {
			return m_pages.size();
		}

		inline bool is_writable(const address_t address) {
			// find existing memory pages
			auto it = m_page_attributes.find(page_number(address));
			if (it != m_page_attributes.end()) {
				return it->second.write;
			}
			// return default
			return PageAttributes().write;
		}

		auto& machine() { return this->m_machine; }
		const auto& machine() const { return this->m_machine; }

		void reset();
		void trap(address_t address, mmio_cb_t callback) {
			this->m_callbacks[address] = callback;
		}

	private:
		inline Page& get_page(const address_t address);
		inline auto& create_attr(const address_t address);
		inline void  set_page_attr(address_t, size_t len, PageAttributes);
		static inline uintptr_t page_number(const address_t address) {
			return address >> Page::SHIFT;
		}
		void binary_loader();

		Machine<W>& m_machine;

		address_t m_start_address = 0;
		address_t m_stack_address = 0;
		size_t    m_pages_total   = 128; // max physical memory usage

		std::unordered_map<address_t, Page> m_pages;
		std::unordered_map<address_t, PageAttributes> m_page_attributes;

		std::vector<uint8_t> m_binary;

		// map of page-indexed trap functions
		// NOTE: uses page-numbers, not byte-addressing
		std::unordered_map<address_t, mmio_cb_t> m_callbacks;
		bool check_trap(address_t address, int size, address_t value) {
			auto it = m_callbacks.find(address);
			if (it == m_callbacks.end()) return true;
			// do the thing
			return it->second(*this, address, size, value);
		}
	};
#include "memory_inline.hpp"
}
