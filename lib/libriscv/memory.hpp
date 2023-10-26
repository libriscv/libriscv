#pragma once
#include "elf.hpp"
#include "page.hpp"
#include <cassert>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include "decoded_exec_segment.hpp"
#include "util/buffer.hpp" // <string>
#include "util/function.hpp"
#ifdef EASTL_ENABLED
#include <stdexcept>
#include <EASTL/fixed_hash_map.h>
#include <EASTL/fixed_vector.h>
#endif

namespace riscv
{
	template<int W> struct Machine;
	struct vBuffer { char* ptr; size_t len; };

	template<int W>
	struct Memory
	{
		using address_t = address_type<W>;
		using mmio_cb_t = Page::mmio_cb_t;
		using page_fault_cb_t = riscv::Function<Page&(Memory&, address_t, bool)>;
		using page_readf_cb_t = riscv::Function<const Page&(const Memory&, address_t)>;
		using page_write_cb_t = riscv::Function<void(Memory&, address_t, Page&)>;
		static constexpr address_t BRK_MAX    = 0x1000000; // Default BRK size

		template <typename T>
		T read(address_t src);

		template <typename T>
		T& writable_read(address_t src);

		template <typename T>
		void write(address_t dst, T value);

		void memzero(address_t dst, size_t len);
		void memset(address_t dst, uint8_t value, size_t len);
		void memcpy(address_t dst, const void* src, size_t);
		void memcpy_unsafe(address_t dst, const void* src, size_t);
		void memcpy(address_t dst, Machine<W>& srcm, address_t src, address_t len);
		void memcpy_out(void* dst, address_t src, size_t) const;
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
		address_t heap_address() const noexcept { return this->m_heap_address; }
		// Simple memory mapping implementation
		address_t mmap_start() const noexcept { return this->m_heap_address + BRK_MAX; }
		const address_t& mmap_address() const noexcept { return m_mmap_address; }
		address_t& mmap_address() noexcept { return m_mmap_address; }
		address_t mmap_allocate(address_t bytes);
		bool mmap_relax(address_t addr, address_t size, address_t new_size);


		auto& machine() { return this->m_machine; }
		const auto& machine() const { return this->m_machine; }
		bool is_forked() const noexcept { return !this->m_original_machine; }

		// Symbol table functions
		address_t resolve_address(std::string_view sym) const;
		address_t resolve_section(const char* name) const;
		// Basic backtraces and symbol lookups
		struct Callsite {
			std::string name = "(null)";
			address_t   address = 0x0;
			uint32_t    offset  = 0x0;
			size_t      size    = 0;
		};
		Callsite lookup(address_t) const;
		void print_backtrace(std::function<void(std::string_view)>, bool ra = true);

		// Counts all the memory used by execute segments, pages, etc.
		size_t memory_usage_total() const;
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
		Page& create_writable_pageno(address_t npage, bool initialize = true);
		void  set_page_attr(address_t, size_t len, PageAttributes);
		std::string get_page_info(address_t addr) const;
		static inline address_t page_number(const address_t address) {
			return address / Page::size();
		}
		// Page creation & destruction
		template <typename... Args>
		Page& allocate_page(address_t page, Args&& ...);
		void  invalidate_cache(address_t pageno, Page*) const;
		void  invalidate_reset_cache() const;
		void  free_pages(address_t, size_t len);
		bool  free_pageno(address_t pageno);
		// Page fault when writing to unused memory
		// The old handler is returned, so it can be restored later.
		page_fault_cb_t set_page_fault_handler(page_fault_cb_t h) {
			auto old_handler = std::move(m_page_fault_handler);
			this->m_page_fault_handler = h;
			return old_handler;
		}


		// Page fault when reading unused memory. Primarily used for
		// pagetable sharing across machines, enabled with RISCV_SHARED_PT.
		page_readf_cb_t set_page_readf_handler(page_readf_cb_t h) {
			auto old_handler = std::move(m_page_readf_handler);
			this->m_page_readf_handler = h;
			return old_handler;
		}
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

		// Custom execute segment, returns page base, final size and execute segment pointer
		DecodedExecuteSegment<W>* exec_segment_for(address_t vaddr);
		const DecodedExecuteSegment<W>* exec_segment_for(address_t vaddr) const;
		const DecodedExecuteSegment<W>& main_execute_segment() const { return m_exec.at(0); }
		DecodedExecuteSegment<W>& create_execute_segment(const MachineOptions<W>&, const void* data, address_t addr, size_t len);
		size_t cached_execute_segments() const noexcept { return m_exec.size(); }
		// Evict newest execute segments until only remaining left
		// Default: Leave only the main execute segment left.
		void evict_execute_segments(size_t remaining_size = 1);

		const auto& binary() const noexcept { return m_binary; }
		void reset();

		bool uses_memory_arena() const noexcept { return this->m_arena != nullptr; }
		void* memory_arena_ptr() const noexcept { return (void *)this->m_arena; }
		address_t memory_arena_size() const noexcept { return this->m_arena_pages * Page::size(); }

		// serializes all the machine state + a tiny header to @vec
		void serialize_to(std::vector<uint8_t>& vec) const;
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
		using ElfRela = typename Elf<W>::Rela;
		template <typename T> T* elf_offset(intptr_t ofs) const {
			return (T*) &m_binary.at(ofs);
		}
		inline const auto* elf_header() const noexcept {
			return elf_offset<const Ehdr> (0);
		}
		const Shdr* section_by_name(const std::string& name) const;
		void dynamic_linking();
		void relocate_section(const char* section_name, const char* symtab);
		const typename Elf<W>::Sym* resolve_symbol(std::string_view name) const;
		const auto* elf_sym_index(const Shdr* shdr, uint32_t symidx) const {
			if (symidx >= shdr->sh_size / sizeof(typename Elf<W>::Sym))
				throw MachineException(INVALID_PROGRAM, "ELF Symtab section index overflow");
			auto* symtab = elf_offset<typename Elf<W>::Sym>(shdr->sh_offset);
			return &symtab[symidx];
		}
		// ELF loader
		void binary_loader(const MachineOptions<W>&);
		void binary_load_ph(const MachineOptions<W>&, const Phdr*);
		void serialize_execute_segment(const MachineOptions<W>&, const Phdr*);
		bool serialize_pages(MemoryArea&, address_t, const char*, size_t, PageAttributes);
		void generate_decoder_cache(const MachineOptions<W>&, DecodedExecuteSegment<W>&);
		// Machine copy-on-write fork
		void machine_loader(const Machine<W>&, const MachineOptions<W>&);

		Machine<W>& m_machine;

		mutable CachedPage<W, const PageData> m_rd_cache;
		mutable CachedPage<W, PageData> m_wr_cache;

#if defined(EASTL_ENABLED)
		eastl::fixed_hash_map<address_t, Page, 128> m_pages;
#else
		std::unordered_map<address_t, Page> m_pages;
#endif

		page_fault_cb_t m_page_fault_handler = nullptr;
		page_write_cb_t m_page_write_handler = default_page_write;
		page_readf_cb_t m_page_readf_handler = default_page_read;

		MemoryArea m_ropages;

		address_t m_start_address = 0;
		address_t m_stack_address = 0;
		address_t m_exit_address  = 0;
		address_t m_mmap_address  = 0;
		address_t m_heap_address  = 0;
		const bool m_original_machine;

		const std::string_view m_binary;

		// Execute segments
#ifdef EASTL_ENABLED
		eastl::fixed_vector<DecodedExecuteSegment<W>, 4> m_exec;
#else
		std::vector<DecodedExecuteSegment<W>> m_exec;
#endif

		// Linear arena at start of memory (mmap-backed)
		PageData* m_arena = nullptr;
		size_t m_arena_pages = 0;
		friend struct CPU<W>;
	};
#include "memory_inline.hpp"
#include "memory_inline_pages.hpp"
#include "memory_helpers_paging.hpp"
}
