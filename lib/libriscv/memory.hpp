#pragma once
#include "elf.hpp"
#include "page.hpp"
#include <cassert>
#include <cstring>
#ifdef RISCV_USE_RH_HASH
#include <robin_hood.h>
#endif
#include "util/buffer.hpp" // <string>

namespace riscv
{
	template<int W> struct Machine;
	template<int W> struct DecoderCache;
	template<int W> struct DecoderData;
	struct vBuffer { char* ptr; size_t len; };

	template<int W>
	struct Memory
	{
		using address_t = address_type<W>;
		using mmio_cb_t = Page::mmio_cb_t;
		using page_fault_cb_t = std::function<Page&(Memory&, address_t)>;
		using page_readf_cb_t = std::function<const Page&(const Memory&, address_t)>;
		using page_write_cb_t = std::function<void(Memory&, address_t, Page&)>;
		static constexpr address_t BRK_MAX    = 0x1000000;
		static constexpr address_t HEAP_START = 0x40000000;

		template <typename T>
		T read(address_t src);

		template <typename T>
		T& writable_read(address_t src);

		template <typename T>
		void write(address_t dst, T value);

		void memzero(address_t dst, size_t len);
		void memset(address_t dst, uint8_t value, size_t len);
		void memcpy(address_t dst, const void* src, size_t);
		void memcpy(address_t dst, Machine<W>& srcm, address_t src, address_t len);
		void memcpy_out(void* dst, address_t src, size_t) const;
		void memcpy_unsafe(address_t dst, const void* src, size_t); // No page protections
		/* Fill an array of buffers pointing to complete guest virtual [addr, len].
		   Throws an exception if there was a protection violation.
		   Returns the number of buffers filled, or an exception if not enough. */
		size_t gather_buffers_from_range(size_t cnt, vBuffer[], address_t addr, size_t len);
		// Gives a chunk-wise view of the data at address, with a callback
		// invocation at each page boundary. @offs is the current byte offset.
		void foreach(address_t addr, size_t len,
			std::function<void(Memory&, address_t offs, const uint8_t*, size_t)> callback);
		void foreach(address_t addr, size_t len,
			std::function<void(const Memory&, address_t offs, const uint8_t*, size_t)>) const;
		// Gives a sequential view of the data at address, with the possibility
		// of optimizing away a copy if the data crosses no page-boundaries.
		void memview(address_t addr, size_t len,
			std::function<void(Memory&, const uint8_t*, size_t)> callback);
		void memview(address_t addr, size_t len,
			std::function<void(const Memory&, const uint8_t*, size_t)> callback) const;
		// Gives const-ref access to pod-type T viewed as sequential memory. (See above)
		template <typename T>
		void memview(address_t addr, std::function<void(const T&)> callback) const;
		// Compare bounded memory
		int memcmp(address_t p1, address_t p2, size_t len) const;
		int memcmp(const void* p1, address_t p2, size_t len) const;
		// Gather fragmented virtual memory into an array of buffers
		riscv::Buffer rvbuffer(address_t addr, size_t len, size_t maxlen = 1 << 24) const;
		// Read a zero-terminated string directly from guests memory
		std::string memstring(address_t addr, size_t maxlen = 1024) const;
		size_t strlen(address_t addr, size_t maxlen = 4096) const;

		address_t start_address() const noexcept { return this->m_start_address; }
		address_t stack_initial() const noexcept { return this->m_stack_address; }
		void set_stack_initial(address_t addr) { this->m_stack_address = addr; }
		address_t exit_address() const noexcept;
		void      set_exit_address(address_t new_exit);
		address_t heap_address() const noexcept { return HEAP_START; }
		address_t& mmap_address() noexcept { return m_mmap_address; }

		auto& machine() { return this->m_machine; }
		const auto& machine() const { return this->m_machine; }

		// Symbol table functions
		address_t resolve_address(const std::string& sym) const;
		address_t resolve_section(const char* name) const;
		// Basic backtraces and symbol lookups
		struct Callsite {
			std::string name = "(null)";
			address_t   address = 0x0;
			uint32_t    offset  = 0x0;
			size_t      size    = 0;
		};
		Callsite lookup(address_t) const;
		void print_backtrace(std::function<void(std::string_view)>);

		// Helpers for memory usage
		size_t pages_active() const noexcept { return m_pages.size(); }
		size_t owned_pages_active() const noexcept;
		// Page handling
		const auto& pages() const noexcept { return m_pages; }
		auto& pages() noexcept { return m_pages; }
		const Page& get_page(address_t) const;
		const Page& get_exec_pageno(address_t npage) const; // throws
		const Page& get_pageno(address_t npage) const;
		const Page& get_readable_pageno(address_t npage) const;
		Page& create_writable_pageno(address_t npage);
		void  set_page_attr(address_t, size_t len, PageAttributes);
		std::string get_page_info(address_t addr) const;
		static inline address_t page_number(const address_t address) {
			return address >> Page::SHIFT;
		}
		// Page creation & destruction
		template <typename... Args>
		Page& allocate_page(address_t page, Args&& ...);
		void  invalidate_cache(address_t pageno, Page*) const;
		void  invalidate_reset_cache() const;
		void  free_pages(address_t, size_t len);
		// Page fault when writing to unused memory
		void set_page_fault_handler(page_fault_cb_t h) { this->m_page_fault_handler = h; }

		// Page fault when reading unused memory. Primarily used for
		// pagetable sharing across machines, enabled with RISCV_SHARED_PT.
		void set_page_readf_handler(page_readf_cb_t h) { this->m_page_readf_handler = h; }
		void reset_page_readf_handler() { this->m_page_readf_handler = default_page_read; }

		// Page write on copy-on-write page
		void set_page_write_handler(page_write_cb_t h) { this->m_page_write_handler = h; }
		static void default_page_write(Memory&, address_t, Page& page);
		static const Page& default_page_read(const Memory&, address_t);
		// NOTE: use print_and_pause() to immediately break!
		void trap(address_t page_addr, mmio_cb_t callback);
		// shared pages (regular pages will have priority!)
		Page&  install_shared_page(address_t pageno, const Page&);
		// create pages for non-owned (shared) memory with given attributes
		void insert_non_owned_memory(
			address_t dst, void* src, size_t size, PageAttributes = {});

		// Returns true if the address is inside the executable code segment
		bool is_executable(address_t addr);

#ifdef RISCV_INSTR_CACHE
		void generate_decoder_cache(const MachineOptions<W>&, address_t pbase, address_t va, size_t len);
		auto* get_decoder_cache() const { return m_exec_decoder; }
#endif

		const auto& binary() const noexcept { return m_binary; }
		void reset();

		bool is_binary_translated() const { return m_bintr_dl != nullptr; }
		void set_binary_translated(void* dl) const { m_bintr_dl = dl; }

		// serializes all the machine state + a tiny header to @vec
		void serialize_to(std::vector<uint8_t>& vec);
		// returns the machine to a previously stored state
		void deserialize_from(const std::vector<uint8_t>&, const SerializedMachine<W>&);

		Memory(Machine<W>&, std::string_view, MachineOptions<W>);
		Memory(Machine<W>&, const Machine<W>&, MachineOptions<W>);
		~Memory();
	private:
		struct MemoryArea {
			address_t begin = 0;
			address_t end = 0;
			std::unique_ptr<Page[]> pages = nullptr;
			std::unique_ptr<uint8_t[]> data = nullptr;
			bool contains(address_t pg) const noexcept { return pg >= begin && pg < end; }
			bool contains(address_t x1, address_t x2) const noexcept {
				return x1 < end && x2 >= begin;
			}
		};
		inline auto& create_attr(const address_t address);
		void clear_all_pages();
		void initial_paging();
		[[noreturn]] static void protection_fault(address_t);
		const PageData& cached_readable_page(address_t, size_t) const;
		PageData& cached_writable_page(address_t);
		// Helpers
		template <typename T>
		static void foreach_helper(T& mem, address_t addr, size_t len,
			std::function<void(T&, address_t, const uint8_t*, size_t)> callback);
		template <typename T>
		static void memview_helper(T& mem, address_t addr, size_t len,
			std::function<void(T&, const uint8_t*, size_t)> callback);
		// ELF stuff
		using Ehdr = typename Elf<W>::Ehdr;
		using Phdr = typename Elf<W>::Phdr;
		using Shdr = typename Elf<W>::Shdr;
		template <typename T> T* elf_offset(intptr_t ofs) const {
			return (T*) &m_binary.at(ofs);
		}
		inline const auto* elf_header() const noexcept {
			return elf_offset<const Ehdr> (0);
		}
		const Shdr* section_by_name(const char* name) const;
		void relocate_section(const char* section_name, const char* symtab);
		const typename Elf<W>::Sym* resolve_symbol(const char* name) const;
		const auto* elf_sym_index(const Shdr* shdr, uint32_t symidx) const {
			assert(symidx < shdr->sh_size / sizeof(typename Elf<W>::Sym));
			auto* symtab = elf_offset<typename Elf<W>::Sym>(shdr->sh_offset);
			return &symtab[symidx];
		}
		// ELF loader
		void binary_loader(const MachineOptions<W>&);
		void binary_load_ph(const MachineOptions<W>&, const Phdr*);
		void serialize_pages(MemoryArea&, address_t, const char*, size_t, PageAttributes);
		// Machine copy-on-write fork
		void machine_loader(const Machine<W>&, const MachineOptions<W>&);

		Machine<W>& m_machine;

		mutable CachedPage<W, const PageData> m_rd_cache;
		mutable CachedPage<W, PageData> m_wr_cache;

#ifdef RISCV_USE_RH_HASH
		robin_hood::unordered_map<address_t, Page> m_pages;
#else
		std::unordered_map<address_t, Page> m_pages;
#endif

		page_fault_cb_t m_page_fault_handler = nullptr;
		page_write_cb_t m_page_write_handler = default_page_write;
		page_readf_cb_t m_page_readf_handler = default_page_read;

#ifdef RISCV_RODATA_SEGMENT_IS_SHARED
		MemoryArea m_ropages;
#endif

		address_t m_start_address = 0;
		address_t m_stack_address = 0;
		address_t m_exit_address  = 0;
		address_t m_mmap_address  = HEAP_START + BRK_MAX;
		const bool m_original_machine;

		const std::string_view m_binary;

		// ELF programs linear .text segment
		std::unique_ptr<uint8_t[]> m_exec_pagedata = nullptr;
		size_t    m_exec_pagedata_size = 0;
		address_t m_exec_pagedata_base = 0;
#ifdef RISCV_INSTR_CACHE
		DecoderData<W>* m_exec_decoder = nullptr;
		DecoderCache<W>* m_decoder_cache = nullptr;
#endif
		mutable void* m_bintr_dl = nullptr;
	};
#include "memory_inline.hpp"
#include "memory_helpers.hpp"
}
