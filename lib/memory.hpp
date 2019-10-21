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
		using mmio_cb_t = delegate<void(Memory&, address_t)>;

		Memory(Machine<W>&, std::vector<uint8_t>);

		template <int SIZE>
		inline auto read(address_t address) {
			auto& page = get_page(address);
			return page.template aligned_value<SIZE>(address & (Page::size()-1));
		}

		template <int SIZE, typename T>
		inline void write(address_t address, T value) {
			auto& page = get_page(address);
			page.template aligned_value<SIZE>(address & (Page::size()-1)) = value;
		}

		inline auto memcpy(address_t dst, const uint8_t* src, size_t);

		address_t start_address() const noexcept { return this->m_start_address; }

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

	private:
		inline Page& get_page(const address_t address);
		inline auto& create_attr(const address_t address);
		inline void  set_page_attr(address_t, size_t len, PageAttributes);
		static inline uintptr_t page_number(const address_t address) {
			return address >> Page::SHIFT;
		}

		Machine<W>& m_machine;

		address_t m_start_address = 0;
		size_t    m_pages_total   = 128; // max physical memory usage
		// map of page-indexed trap functions
		// NOTE: uses page-numbers, not byte-addressing
		std::unordered_map<address_t, mmio_cb_t> m_callbacks;

		std::unordered_map<address_t, Page> m_pages;
		std::unordered_map<address_t, PageAttributes> m_page_attributes;
	};

	template <int W>
	inline Page& Memory<W>::get_page(const address_t address)
	{
		const auto page = page_number(address);
		// find existing memory pages
		auto it = m_pages.find(page);
		if (it != m_pages.end()) {
			return it->second;
		}
		// creates pages on-demand
		if (m_pages.size() < m_pages_total) {
			auto it = m_pages.emplace(
				std::piecewise_construct,
				std::forward_as_tuple(page),
				std::forward_as_tuple());
			return it.first->second;
		}
		throw std::runtime_error("Out of memory");
	}

	template <int W>
	inline auto& Memory<W>::create_attr(const address_t address)
	{
		const auto page = page_number(address);
		// find existing memory pages
		auto it = m_page_attributes.find(page);
		if (it != m_page_attributes.end()) {
			return it->second;
		}
		// creates pages on-demand
		if (m_page_attributes.size() < m_pages_total) {
			auto it = m_page_attributes.emplace(
				std::piecewise_construct,
				std::forward_as_tuple(page),
				std::forward_as_tuple());
			return it.first->second;
		}
		throw std::runtime_error("Out of memory");
	}

	template <int W>
	auto Memory<W>::memcpy(address_t dst, const uint8_t* src, size_t len)
	{
		while (len > 0)
		{
			const size_t offset = dst & (Page::size()-1);
			const size_t remaining = (offset == 0) ? Page::size() : (Page::size() - offset);
			const size_t size = std::min(remaining, len);
			auto& page = this->get_page(dst);
			std::memcpy(page.data(), src, size);

			dst += size;
			src += size;
			len -= size;
		}
		return dst;
	}

	template <int W>
	void Memory<W>::set_page_attr(address_t dst, size_t len, PageAttributes options)
	{
		while (len > 0)
		{
			const size_t size = std::min(Page::size(), len);
			auto& attr = this->create_attr(dst);
			attr = options;

			dst += size;
			len -= size;
		}
	}
}
