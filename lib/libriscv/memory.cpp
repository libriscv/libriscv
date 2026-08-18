#include "machine.hpp"

#include "decoder_cache.hpp"
#include "internal_common.hpp"
#include <algorithm>
#include <inttypes.h>
#if defined(__linux__) || defined(__FreeBSD__) || defined(__wasm__)
#define DEMANGLE_ENABLED
#include <sys/mman.h>
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
extern "C" char *
__cxa_demangle(const char *name, char *buf, size_t *n, int *status);
#endif

namespace riscv
{
	// The arena data pointer is offset OVERALLOCATE into the mapping, so the
	// mapping needs one extra page on each side of the guest address space in
	// order to absorb the tail of multi-byte accesses at the last guest
	// address without bounds-checking the access size on every access.
	[[maybe_unused]] static constexpr uint64_t UNBOUNDED_ARENA_SIZE = (1ULL << encompassing_Nbit_arena) + 2 * Page::size();

	// True when every byte in the range is zero.
	[[maybe_unused]] static bool is_zeroed(const uint8_t* data, size_t len) noexcept
	{
		for (size_t i = 0; i < len; i++)
			if (data[i] != 0)
				return false;
		return true;
	}

	template <int W>
	Memory<W>::Memory(Machine<W>& mach, std::string_view bin,
					MachineOptions<W> options)
		: m_machine{mach},
		  m_original_machine {true},
		  m_binary {bin}
	{
#ifdef RISCV_VIRTUAL_PAGING
		// Bound the page table, including attribute-only pages that are
		// created outside of the page fault handler (eg. mmap, mprotect)
		if (options.memory_max != 0)
		{
			const size_t pgmax = std::max(size_t(1), size_t(options.memory_max / Page::size()));
			this->m_pages_max = (pgmax <= size_t(-1) / PAGE_TABLE_OVERCOMMIT)
				? pgmax * PAGE_TABLE_OVERCOMMIT : size_t(-1);
		}

		if (options.page_fault_handler != nullptr)
		{
			this->m_page_fault_handler = std::move(options.page_fault_handler);
		}
		else
#endif
		if (options.memory_max != 0)
		{
			const address_t pages_max = options.memory_max / Page::size();
			assert(pages_max >= 1);

			if (options.use_memory_arena)
			{
#if defined(__linux__) || defined(__FreeBSD__)
				if constexpr (encompassing_Nbit_arena != 0)
				{
					static_assert(flat_readwrite_arena || encompassing_Nbit_arena == 0,
						"N-bit encompassing arena requires flat_readwrite_arena to be enabled");

					// Allocate a complete N-bit arena, covering the entire N-bit address space
					// Add 1 extra page to avoid having to bounds-check memory accesses
					// TODO: Allocate unpresent pages for the whole address space,
					// and only allocate real memory according to pages_max. Then handle
					// page faults for the rest of the address space using userfaultfd.
					auto* base_ptr = (uint8_t *)mmap(NULL, UNBOUNDED_ARENA_SIZE, PROT_READ | PROT_WRITE,
						MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
					if (UNLIKELY(base_ptr == MAP_FAILED)) {
						// We probably reached a limit on the number of mappings
						this->m_arena.data = nullptr;
						throw MachineException(OUT_OF_MEMORY, "Out of memory", UNBOUNDED_ARENA_SIZE);
					}
					// Adjust pointer forward by OVERALLOCATE to provide over-allocation on both sides
					// while keeping arena at same logical address (relative to zero)
					this->m_arena.data = (PageData *)(base_ptr + Memory::OVERALLOCATE);
					this->m_arena.pages = (1ULL << encompassing_Nbit_arena) / Page::size();
				} else {
				// Over-allocate by one page on each side in order to avoid
				// bounds-checking with size: the front page absorbs accesses
				// before the arena, and the tail page absorbs the tail of
				// multi-byte accesses at the last guest address.
				const size_t len = (pages_max + 2) * Page::size();
				auto* base_ptr = (uint8_t *)mmap(NULL, len, PROT_READ | PROT_WRITE,
						MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
					this->m_arena.pages = pages_max;
					// mmap() returns MAP_FAILED (-1) when mapping fails
					if (UNLIKELY(base_ptr == MAP_FAILED)) {
						this->m_arena.data = nullptr;
						this->m_arena.pages = 0;
					} else {
						// Adjust pointer forward by OVERALLOCATE to provide over-allocation on both sides
						// while keeping arena at same logical address (relative to zero)
						this->m_arena.data = (PageData *)(base_ptr + Memory::OVERALLOCATE);
					}
				}
#else
				if constexpr (encompassing_Nbit_arena != 0)
				{
					// Allocate a complete N-bit arena, covering the entire N-bit address space
					// Add 1 extra page to avoid having to bounds-check memory accesses
					auto* base_ptr = (uint8_t *)new PageData[UNBOUNDED_ARENA_SIZE / Page::size()];
					// Adjust pointer forward by OVERALLOCATE to provide over-allocation on both sides
					// while keeping arena at same logical address (relative to zero)
					this->m_arena.data = (PageData *)(base_ptr + Memory::OVERALLOCATE);
					this->m_arena.pages = (1ULL << encompassing_Nbit_arena) / Page::size();
				} else {
				// TODO: XXX: Investigate if this is a time sink
				auto* base_ptr = (uint8_t *)new PageData[pages_max + 2];
				// Adjust pointer forward by OVERALLOCATE to provide over-allocation on both sides
				// while keeping arena at same logical address (relative to zero)
				this->m_arena.data = (PageData *)(base_ptr + Memory::OVERALLOCATE);
				this->m_arena.pages = pages_max;
			}
#endif
			}

#ifdef RISCV_VIRTUAL_PAGING
			if (this->m_arena.pages > 0)
			{
				// There is now a sequential arena, but we should make room for
				// some pages that can appear anywhere in the address space.
				const unsigned anywhere_pages = pages_max / 2;
				this->m_page_fault_handler =
				[anywhere_pages] (auto& mem, const address_t page, bool init) -> Page&
				{
					if (mem.pages_active() < anywhere_pages || mem.owned_pages_below(anywhere_pages))
					{
						// Within linear arena at the start
						if (page < mem.m_arena.pages)
						{
							const PageAttributes attr {
								.read  = true,
								.write = true,
								.non_owning = true
							};
							return mem.allocate_page(page, attr, &mem.m_arena.data[page]);
						}
						// Create page on-demand
						return mem.allocate_page(page,
							init ? PageData::INITIALIZED : PageData::UNINITIALIZED);
					}
					// Out of memory, which is (2 + 1) * anywhere_pages
					throw MachineException(OUT_OF_MEMORY, "Out of memory", anywhere_pages * 3);
				};
			} else {
				this->m_page_fault_handler =
					[pages_max](auto &mem, const address_t page, bool init) -> Page &
				{
					if (mem.pages_active() < pages_max || mem.owned_pages_below(pages_max))
					{
						// Create page on-demand
						return mem.allocate_page(page,
							init ? PageData::INITIALIZED : PageData::UNINITIALIZED);
					}
					throw MachineException(OUT_OF_MEMORY, "Out of memory", pages_max);
				};
			}
#else
			if (UNLIKELY(this->m_arena.pages == 0)) {
				throw MachineException(OUT_OF_MEMORY,
					"Virtual paging is disabled but arena allocation failed", 0);
			}
#endif // RISCV_VIRTUAL_PAGING
		} else {
			throw MachineException(OUT_OF_MEMORY, "Max memory was zero", 0);
		}
		if (!m_binary.empty()) {
#ifdef RISCV_VIRTUAL_PAGING
			// Add a zero-page at the start of address space
			this->initial_paging();
#endif
			// load ELF binary into virtual memory
			this->binary_loader(options);
		}
	}
	template <int W>
	Memory<W>::Memory(Machine<W>& mach, const Machine<W>& other, MachineOptions<W> options)
	  : m_machine{mach},
		m_original_machine {false},
		m_binary{other.memory.binary()}
	{
#ifdef RISCV_EXT_ATOMICS
		this->m_atomics = other.memory.m_atomics;
#endif
		this->machine_loader(other, options);
	}

	template <int W>
	Memory<W>::~Memory()
	{
#ifdef RISCV_VIRTUAL_PAGING
		try {
			this->clear_all_pages();
		} catch (...) {}
#endif
		// Potentially deallocate execute segments that are no longer referenced
		this->evict_execute_segments();
		// only the original machine owns arena
		if (this->m_arena.data != nullptr && !is_forked()) {
#if defined(__linux__) || defined(__FreeBSD__)
			// Adjust back to the original base pointer (subtract OVERALLOCATE)
			auto* base_ptr = (uint8_t *)this->m_arena.data - Memory::OVERALLOCATE;
			if constexpr (riscv::encompassing_Nbit_arena != 0)
			{
				// munmap() the entire address space
				munmap(base_ptr, UNBOUNDED_ARENA_SIZE);
			} else {
				munmap(base_ptr, (this->m_arena.pages + 2) * Page::size());
			}
#else
			// Adjust back to the original base pointer (subtract OVERALLOCATE)
			auto* base_ptr = (PageData *)((uint8_t *)this->m_arena.data - Memory::OVERALLOCATE);
			delete[] base_ptr;
#endif
		}
	}

	template <int W> RISCV_INTERNAL
	void Memory<W>::reset()
	{
		// Hard to support because of things like
		// serialization, machine options and machine forks
	}

#ifdef RISCV_VIRTUAL_PAGING
	template <int W>
	void Memory<W>::clear_all_pages()
	{
		this->m_pages.clear();
		this->invalidate_reset_cache();
	}

	template <int W> RISCV_INTERNAL
	void Memory<W>::initial_paging()
	{
		if (m_pages.find(0) == m_pages.end()) {
			// add a guard page to catch zero-page accesses
			install_shared_page(0, Page::guard_page());
		}
	}
#endif // RISCV_VIRTUAL_PAGING

	template <int W> RISCV_INTERNAL
	void Memory<W>::binary_load_ph(const MachineOptions<W>& options,
		const typename Elf::ProgramHeader* hdr, const address_t vaddr)
	{
		const auto* src = m_binary.data() + hdr->p_offset;
		const size_t len = hdr->p_filesz;
		if (m_binary.size() <= hdr->p_offset ||
			hdr->p_offset + len < hdr->p_offset)
		{
			throw MachineException(INVALID_PROGRAM, "Bogus ELF program segment offset");
		}
		if (m_binary.size() < hdr->p_offset + len) {
			throw MachineException(INVALID_PROGRAM, "Not enough room for ELF program segment");
		}
		if (vaddr + len < vaddr) {
			throw MachineException(INVALID_PROGRAM, "Bogus ELF segment virtual base");
		}

		if (options.verbose_loader) {
		printf("* Loading program of size %zu from %p to virtual %p -> %p\n",
				len, src, (void*)uintptr_t(vaddr), (void*)uintptr_t(vaddr + len));
		}
		// Serialize pages cannot be called with len == 0,
		// and there is nothing further to do.
		if (UNLIKELY(len == 0))
			return;

		// segment permissions
		const PageAttributes attr {
			 .read  = (hdr->p_flags & Elf::PF_R) != 0,
			 .write = (hdr->p_flags & Elf::PF_W) != 0,
			 .exec  = (hdr->p_flags & Elf::PF_X) != 0
		};
		if (options.verbose_loader) {
		printf("* Program segment readable: %d writable: %d  executable: %d\n",
				attr.read, attr.write, attr.exec);
		}

		// Nothing more to do here, if execute-only
		if (attr.exec && !attr.read)
			return;
		// We would normally never allow this
		if (attr.exec && attr.write) {
			if (!options.allow_write_exec_segment) {
				throw MachineException(INVALID_PROGRAM,
					"Insecure ELF has writable executable code (Disable check in MachineOptions)");
			}
		}
		// In some cases we want to enforce execute-only
		if (attr.exec && (attr.read || attr.write)) {
			if (options.enforce_exec_only) {
				throw MachineException(INVALID_PROGRAM, "Execute segment must be execute-only");
			}
		}

		if constexpr (!virtual_paging_enabled) {
			// Write directly to the arena (rodata boundary set after copy)
			if (UNLIKELY(vaddr + len > memory_arena_size()))
				throw MachineException(INVALID_PROGRAM, "ELF segment exceeds arena size");
			// A shared image already holds the bytes below its end, read-only
			const address_t shared_end = this->shared_rodata_end();
			const size_t skip = (vaddr < shared_end)
				? std::min(len, size_t(shared_end - vaddr)) : 0u;
			if (skip < len)
				std::memcpy(&((char*)m_arena.data)[vaddr + skip], src + skip, len - skip);
		} else {
			// Load into virtual memory
			this->memcpy(vaddr, src, len);
		}

		// Track rodata boundary (set after memcpy so arena writes succeed)
		if (attr.read && !attr.write && uses_flat_memory_arena()) {
			this->m_arena.initial_rodata_end =
				std::max(m_arena.initial_rodata_end, static_cast<address_t>(vaddr + len));
		}

		if (options.protect_segments) {
			this->set_page_attr(vaddr, len, attr);
		}
		else {
			// this might help execute simplistic barebones programs
			this->set_page_attr(vaddr, len, {
				 .read = true, .write = true, .exec = true
			});
		}
	}

	template <int W> RISCV_INTERNAL
	void Memory<W>::serialize_execute_segment(
		const MachineOptions<W>& options, const typename Elf::ProgramHeader* hdr, address_t vaddr)
	{
		// The execute segment:
		size_t exlen = hdr->p_filesz;
		const char* data = m_binary.data() + hdr->p_offset;

		// Zig's ELF writer is insane, so we add an option to disable .text section segment reduction.
		if (W <= 8 && !options.ignore_text_section)
		{
			// Look for a .text section inside this segment:
			const auto* texthdr = section_by_name_validated(".text");
			if (texthdr != nullptr
				// Validate that the .text section is inside this
				// execute segment.
				&& texthdr->sh_addr >= vaddr && texthdr->sh_size <= exlen
				&& texthdr->sh_addr + texthdr->sh_size <= vaddr + exlen)
			{
				data = m_binary.data() + texthdr->sh_offset;
				vaddr = this->elf_base_address(texthdr->sh_addr);
				exlen = texthdr->sh_size;
			}
			//printf("* Found .text section inside segment: %p -> %p\n",
			//	(void*)uintptr_t(vaddr), (void*)uintptr_t(vaddr + exlen));
		}

		// Create an *initial* execute segment
		auto& exec_segment =
			this->create_execute_segment(options, data, vaddr, exlen, true);
		// Set the segment as execute-only when R|W are not set
		exec_segment.set_execute_only((hdr->p_flags & (Elf::PF_R | Elf::PF_W)) == 0);
		// Select the first execute segment
		if (machine().cpu.current_execute_segment().empty())
			machine().cpu.set_execute_segment(exec_segment);
	}

	// ELF32 and ELF64 loader
	template <int W> RISCV_INTERNAL
	void Memory<W>::prepare_shared_rodata(const MachineOptions<W>& options,
		const typename Elf::Header* elf)
	{
		// With virtual paging the arena is loaned out through the page table,
		// instead of being the only home of guest memory
		if constexpr (virtual_paging_enabled)
			return;
		// An encompassing arena stores without consulting the write boundary,
		// so a read-only mapping turns an ordinary guest store into a crash
		if constexpr (encompassing_Nbit_arena != 0)
			return;

		if (!options.use_shared_rodata || !options.load_program)
			return;
		if (!this->uses_flat_memory_arena() || !shared_rodata_supported())
			return;

		// One read-only segment, and where in the binary its bytes come from.
		struct RoSeg {
			address_t vaddr;
			uint64_t  offset;
			uint64_t  size;
		};
		std::vector<RoSeg> segments;

		const auto* phdr = (typename Elf::ProgramHeader*) (m_binary.data() + elf->e_phoff);
		address_t rodata_end = 0;
		address_t writable_begin = ~address_t(0);

		for (const auto* hdr = phdr; hdr < phdr + elf->e_phnum; hdr++)
		{
			if (hdr->p_type != Elf::PT_LOAD)
				continue;
			const address_t vaddr = this->elf_base_address(hdr->p_vaddr);

			// A writable segment can sit anywhere: remember the lowest one, as
			// its first page can never be shared
			if (hdr->p_flags & Elf::PF_W) {
				writable_begin = std::min(writable_begin, vaddr);
				continue;
			}
			// The conditions in binary_load_ph() that extend the read-only
			// boundary: loaded, readable and not writable
			if (hdr->p_filesz == 0 || (hdr->p_flags & Elf::PF_R) == 0)
				continue;
			// binary_load_ph() rejects segments outside the binary too, but only
			// runs after us. 64-bit arithmetic, as the sum of two 32-bit ELF
			// fields wraps in a 32-bit binary.
			const uint64_t file_offset = uint64_t(hdr->p_offset);
			const uint64_t file_end = file_offset + uint64_t(hdr->p_filesz);
			if (file_end < file_offset || file_end > m_binary.size())
				continue;
			if (vaddr + hdr->p_filesz < vaddr)
				continue;

			rodata_end = std::max(rodata_end, address_t(vaddr + hdr->p_filesz));
			segments.push_back(RoSeg {
				.vaddr = vaddr, .offset = file_offset, .size = uint64_t(hdr->p_filesz)
			});
		}

		if (rodata_end == 0)
			return;
		// Interleaved segments, which a single write boundary cannot express
		if (writable_begin < rodata_end)
			return;

		// Only whole pages can be shared
		static constexpr address_t page_mask = ~address_t(Page::size()-1);
		address_t shared_end = rodata_end & page_mask;
		if (writable_begin != ~address_t(0))
			shared_end = std::min(shared_end, address_t(writable_begin & page_mask));

		if (shared_end == 0 || shared_end > this->memory_arena_size())
			return;

		// Sorted by address, so that the layout does not depend on the order of
		// the program headers, and verify() below can walk it in one pass
		std::sort(segments.begin(), segments.end(),
			[] (const RoSeg& a, const RoSeg& b) { return a.vaddr < b.vaddr; });

		std::vector<RoSeg> shared_segments;
		std::vector<RodataSegment> layout;
		for (const auto& seg : segments) {
			if (seg.vaddr >= shared_end)
				continue;
			const uint64_t size = std::min(seg.size, uint64_t(shared_end - seg.vaddr));
			shared_segments.push_back(RoSeg { seg.vaddr, seg.offset, size });
			layout.push_back(RodataSegment { uint64_t(seg.vaddr), size });
		}

		// Nothing of the program itself lands inside the shared region
		if (layout.empty())
			return;

		this->m_rodata_key = RodataKey {
			.rodata_end = uint64_t(shared_end),
			.arena_size = uint64_t(this->memory_arena_size()),
			.segments   = std::move(layout),
		};

		// The layout only narrows the search: an image belongs to this program
		// only when every shared byte matches, gaps included. Anything weaker
		// would hand one programs memory to another.
		auto verify = [&] (const uint8_t* image, size_t len) -> bool {
			if (len != size_t(shared_end))
				return false;
			uint64_t pos = 0;
			for (const auto& seg : shared_segments) {
				if (seg.vaddr < pos || seg.vaddr + seg.size > len)
					return false;
				if (!is_zeroed(image + pos, size_t(seg.vaddr - pos)))
					return false;
				if (std::memcmp(image + seg.vaddr, m_binary.data() + seg.offset, seg.size) != 0)
					return false;
				pos = seg.vaddr + seg.size;
			}
			return is_zeroed(image + pos, size_t(len - pos));
		};

		// Mapping it now also lets the loader skip the segments it covers
		auto image = find_shared_rodata_image(this->m_rodata_key, verify);
		if (image == nullptr)
			return;
		if (!image->map_over(this->m_arena.data))
			return;
		this->m_rodata_image = std::move(image);

		if (UNLIKELY(options.verbose_loader)) {
			printf("* Attached shared read-only memory of %zu bytes\n", size_t(shared_end));
		}
	}

	template <int W> RISCV_INTERNAL
	void Memory<W>::publish_shared_rodata()
	{
		// Already sharing, or not a candidate at all
		if (this->m_rodata_image != nullptr || this->m_rodata_key.rodata_end == 0)
			return;

		// The arena now holds the finished read-only region, relocations and all
		const size_t len = size_t(this->m_rodata_key.rodata_end);
		auto image = create_shared_rodata_image(this->m_rodata_key, this->m_arena.data, len);
		if (image == nullptr)
			return;

		// Invisible to us: the bytes are the ones we just copied out of here
		if (image->map_over(this->m_arena.data))
			this->m_rodata_image = std::move(image);
	}

	template <int W> RISCV_INTERNAL
	void Memory<W>::binary_loader(const MachineOptions<W>& options)
	{
		static constexpr uint32_t ELFHDR_FLAGS_RVC = 0x1;
		static constexpr uint32_t ELFHDR_FLAGS_RVE = 0x8;

		if (UNLIKELY(m_binary.size() < sizeof(typename Elf::Header))) {
			throw MachineException(INVALID_PROGRAM, "ELF program too short");
		}
		if (UNLIKELY(!Elf::validate(m_binary))) {
			if constexpr (W == 4)
				throw MachineException(INVALID_PROGRAM, "Invalid ELF header! Expected a 32-bit RISC-V ELF binary");
			else if constexpr (W == 8)
				throw MachineException(INVALID_PROGRAM, "Invalid ELF header! Expected a 64-bit RISC-V ELF binary");
			else if constexpr (W == 16)
				throw MachineException(INVALID_PROGRAM, "Invalid ELF header! Expected a 128-bit RISC-V ELF binary");
			else
				throw MachineException(INVALID_PROGRAM, "Invalid ELF header! Expected a RISC-V ELF binary");
		}

		const auto* elf = (typename Elf::Header*) m_binary.data();
		const bool is_static = elf->e_type == Elf::Header::ET_EXEC;
		this->m_is_dynamic   = elf->e_type == Elf::Header::ET_DYN;
		if (UNLIKELY(!is_static && !m_is_dynamic)) {
			throw MachineException(INVALID_PROGRAM, "ELF program is not an executable type. Trying to load an object file?");
		}
		if (UNLIKELY(elf->e_machine != Elf::Header::EM_RISCV)) {
			throw MachineException(INVALID_PROGRAM, "ELF program is not a RISC-V executable. Wrong architecture.");
		}
		if (UNLIKELY((elf->e_flags & ELFHDR_FLAGS_RVC) != 0 && !compressed_enabled)) {
			throw MachineException(INVALID_PROGRAM, "ELF is a RISC-V RVC executable, however C-extension is not enabled.");
		}
		if (UNLIKELY((elf->e_flags & ELFHDR_FLAGS_RVE) != 0)) {
			throw MachineException(INVALID_PROGRAM, "ELF is a RISC-V RVE executable, however E-extension is not supported.");
		}

		// Enumerate & validate loadable segments
		const auto program_headers = elf->e_phnum;
		if (UNLIKELY(program_headers <= 0)) {
			throw MachineException(INVALID_PROGRAM, "ELF with no program-headers");
		}
		if (UNLIKELY(program_headers >= 16)) {
			throw MachineException(INVALID_PROGRAM, "ELF with too many program-headers");
		}
		if (UNLIKELY(elf->e_phoff > 0x4000)) {
			throw MachineException(INVALID_PROGRAM, "ELF program-headers have bogus offset");
		}
		if (UNLIKELY(elf->e_phoff + program_headers * sizeof(typename Elf::ProgramHeader) > m_binary.size())) {
			throw MachineException(INVALID_PROGRAM, "ELF program-headers are outside the binary");
		}
		if (UNLIKELY(elf->e_phoff % alignof(typename Elf::ProgramHeader) != 0)) {
			throw MachineException(INVALID_PROGRAM, "ELF program-headers are not aligned");
		}

		// Load program segments
		const auto* phdr = (typename Elf::ProgramHeader*) (m_binary.data() + elf->e_phoff);
		std::vector<const typename Elf::ProgramHeader*> execute_segments;

		// is_dynamic() is used to determine the ELF base address
		this->m_start_address = this->elf_base_address(elf->e_entry);
		this->m_heap_address = 0;

		// Before loading, so that the loader can skip the shared segments
		this->prepare_shared_rodata(options, elf);

		for (const auto* hdr = phdr; hdr < phdr + program_headers; hdr++)
		{
			const address_t vaddr = this->elf_base_address(hdr->p_vaddr);

			// Detect overlapping segments
			for (const auto* ph = phdr; ph < hdr; ph++) {
				const address_t ph_vaddr = this->elf_base_address(ph->p_vaddr);

				if (hdr->p_type == Elf::PT_LOAD && ph->p_type == Elf::PT_LOAD)
				if (ph_vaddr < vaddr + hdr->p_filesz &&
					ph_vaddr + ph->p_filesz > vaddr) {
					// Normally we would not care, but no normal ELF
					// has overlapping segments, so treat as bogus.
					throw MachineException(INVALID_PROGRAM, "Overlapping ELF segments");
				}
			}

			switch (hdr->p_type)
			{
				case Elf::PT_LOAD:
					// loadable program segments
					if (options.load_program) {
						binary_load_ph(options, hdr, vaddr);
						if (hdr->p_flags & Elf::PF_X) {
							execute_segments.push_back(hdr);
						}
					}
					break;
				case Elf::PT_GNU_STACK:
					// This seems to be a mark for executable stack. Big NO!
					break;
				case Elf::PT_GNU_RELRO:
					/*this->set_page_attr(vaddr, hdr->p_memsz, {
						.read  = (hdr->p_flags & PF_R) != 0,
						.write = (hdr->p_flags & PF_W) != 0,
						.exec  = (hdr->p_flags & PF_X) != 0,
					});*/
					break;
			}

			address_t endm = vaddr + hdr->p_memsz;
			endm += Page::size()-1; endm &= ~address_t(Page::size()-1);
			if (this->m_heap_address < endm)
				this->m_heap_address = endm;
		}

		// The base mmap address starts at heap start + BRK_MAX
		// TODO: We should check if the heap starts too close to the end
		// of the address space now, and move it around if necessary.
		this->m_mmap_address = m_heap_address + BRK_MAX;
		this->m_brk_address  = m_heap_address;

		// Default stack
		this->m_stack_address = mmap_allocate(options.stack_size) + options.stack_size;

		if (!options.default_exit_function.empty())
		{
			// It is slightly faster to set a custom exit function, in order
			// to avoid changing execute segment (slow-path) to exit.
			auto potential_exit_addr = this->resolve_address(options.default_exit_function);
			if (potential_exit_addr != 0x0) {
				this->m_exit_address = potential_exit_addr;
				if (UNLIKELY(options.verbose_loader)) {
					printf("* Using program-provided exit function at %p\n",
						(void*)uintptr_t(this->exit_address()));
				}
			}
		}
		// Default fallback: Install our own exit function as a separate execute segment
		if (this->m_exit_address == 0x0)
		{
			auto host_page = this->mmap_allocate(Page::size());
			this->m_exit_address = host_page;
			this->m_sigreturn_address = host_page + SIGRETURN_OFFSET;
#ifdef RISCV_VIRTUAL_PAGING
			// Insert host code page, with exit function, enabling VM calls.
			this->install_shared_page(page_number(host_page), Page::host_page());
#else
			// Write exit code directly into the arena
			static constexpr uint8_t exit_code[] = {
				0x73, 0x00, 0xf0, 0x7f, // STOP: 0x7ff00073
				0x6f, 0xf0, 0xdf, 0xff, // JMP -4: 0xffdff06f
				// Signal trampoline, at Memory::SIGRETURN_OFFSET
				0x93, 0x08, 0xb0, 0x08, // LI a7, 139 (rt_sigreturn)
				0x73, 0x00, 0x00, 0x00, // ECALL
				0x6f, 0xf0, 0xdf, 0xff, // JMP -4: 0xffdff06f
			};
			if (host_page + sizeof(exit_code) <= memory_arena_size()) {
				std::memcpy(&((uint8_t*)m_arena.data)[host_page], exit_code, sizeof(exit_code));
			}
			// Create a small execute segment for the exit function
			this->create_execute_segment(options,
				exit_code, host_page, sizeof(exit_code), false);
#endif
		}

		if (this->uses_flat_memory_arena() && this->memory_arena_size() >= m_arena.initial_rodata_end) {
			this->m_arena.read_boundary = std::min(this->memory_arena_size(), size_t(this->memory_arena_size() - RWREAD_BEGIN));
			this->m_arena.write_boundary = std::min(this->memory_arena_size(), size_t(this->memory_arena_size() - m_arena.initial_rodata_end));
		} else {
			this->m_arena.initial_rodata_end = 0;
		}

		// Now that we know the boundries of the program, generate
		// efficient execute segments (if loadable).
		if (options.load_program) {
			for (auto* hdr : execute_segments) {
				const address_t vaddr = this->elf_base_address(hdr->p_vaddr);

				serialize_execute_segment(options, hdr, vaddr);
			}
			if constexpr (W <= 8) {
				if (this->m_is_dynamic) {
					this->dynamic_linking(*elf);
				}
			}
		}

		// Offer the region to later machines, unless already sharing one
		const bool was_sharing = this->shared_rodata_end() != 0;
		this->publish_shared_rodata();

		if (UNLIKELY(options.verbose_loader) && !was_sharing && this->shared_rodata_end() != 0) {
			printf("* Created shared read-only memory of %zu bytes\n",
				size_t(this->shared_rodata_end()));
		}

		if (UNLIKELY(options.verbose_loader)) {
			printf("* Entry is at %p\n",
				(void*)uintptr_t(this->start_address()));
		}
	}

	template <int W> RISCV_INTERNAL
	void Memory<W>::machine_loader(
		const Machine<W>& master, const MachineOptions<W>& options)
	{
#ifdef RISCV_VIRTUAL_PAGING
		this->m_pages_max = master.memory.m_pages_max;

		if (options.minimal_fork == false)
		{
			this->m_page_fault_handler = master.memory.m_page_fault_handler;

			// Hardly any pages are dont_fork, so we estimate that
			// all master pages will be loaned.
			m_pages.reserve(master.memory.pages().size());

			for (const auto& it : master.memory.pages())
			{
				const auto& page = it.second;
				// Skip pages marked as dont_fork
				if (page.attr.dont_fork) continue;
				// Make every page non-owning
				auto attr = page.attr;
				if (attr.write) {
					attr.write = false;
					attr.is_cow = true;
				}
				attr.non_owning = true;
				m_pages.try_emplace(
					it.first,
					attr, page.m_page.get()
				);
			}
		}
#else
		(void)options;
#endif
		this->m_start_address = master.memory.m_start_address;
		this->m_stack_address = master.memory.m_stack_address;
		this->m_exit_address = master.memory.m_exit_address;
		this->m_sigreturn_address = master.memory.m_sigreturn_address;
		this->m_heap_address = master.memory.m_heap_address;
		this->m_brk_address  = master.memory.m_brk_address;
		this->m_mmap_address = master.memory.m_mmap_address;
		this->m_mmap_cache   = master.memory.m_mmap_cache;

		// Reference the same execute segments
		this->m_main_exec_segment = master.memory.m_main_exec_segment;
		this->m_exec = master.memory.m_exec;

		if (options.use_memory_arena) {
			// A fork references the arena of its master, image and all
			this->m_rodata_key = master.memory.m_rodata_key;
			this->m_rodata_image = master.memory.m_rodata_image;
			this->m_arena.data = master.memory.m_arena.data;
			this->m_arena.pages = master.memory.m_arena.pages;
			this->m_arena.read_boundary = master.memory.m_arena.read_boundary;
			this->m_arena.write_boundary = master.memory.m_arena.write_boundary;
			this->m_arena.initial_rodata_end = master.memory.m_arena.initial_rodata_end;
		}

		// invalidate all cached pages, because references are invalidated
		this->invalidate_reset_cache();
	}

	template <int W>
	typename Memory<W>::Callsite Memory<W>::lookup(address_t address) const
	{
		if (!Elf::validate(this->m_binary))
			return {};
		// backtrace can sometimes find null addresses
		if (address == 0x0) return {};

		// Add the correct offset to address for dynamically loaded programs
		address = this->elf_base_address(address);

		const auto result =
			[] (const char* symname, address_t addr, const auto& sym)
		{
			if (symname == nullptr) symname = "(invalid)";
			std::string result;
#ifdef DEMANGLE_ENABLED
			if (char* dma = __cxa_demangle(symname, nullptr, nullptr, nullptr); dma != nullptr) {
				result = dma;
				free(dma);
			} else {
				result = symname;
			}
#else
			result = symname;
#endif
			return Callsite {
				.name = result,
				.address = static_cast<address_t>(sym.st_value),
				.offset = (uint32_t) (addr - sym.st_value),
				.size   = size_t(sym.st_size)
			};
		};

		const typename Elf::Sym* best = nullptr;
		const char* best_name = nullptr;
		for_each_symbol([&] (const auto& sym, const char* symname) {
			if (Elf::SymbolType(sym.st_info) != Elf::STT_FUNC) return;

			if (address >= sym.st_value && (!best || sym.st_value > best->st_value)) {
				// best guess (symbol + 0xOff)
				best = &sym;
				best_name = symname;
			}
		});
		if (best)
			return result(best_name, address, *best);
		return {};
	}
	template <int W>
	void Memory<W>::print_backtrace(
		std::function<void(std::string_view)> print_function, bool ra) const
	{
		auto print_trace =
			[this, print_function] (const int N, const address_type<W> addr) {
				// get information about the callsite
				const auto site = this->lookup(addr);
				if (site.address == 0 && site.offset == 0 && site.size == 0) {
					// if there is nothing to print, indicate that this is
					// an unknown/empty location by "printing" a zero-length string.
					print_function({});
					return;
				}

				// write information directly to stdout
				char buffer[8192];
				int len = 0;
				if (N >= 0) {
					len = snprintf(&buffer[len], sizeof(buffer)-len,
						"[%d] ", N);
				}
				if constexpr (W == 4) {
					len += snprintf(&buffer[len], sizeof(buffer)-len,
						"0x%08" PRIx32 " + 0x%.3" PRIx32 ": %s",
						site.address, site.offset, site.name.c_str());
				} else if constexpr (W == 8) {
					len += snprintf(&buffer[len], sizeof(buffer)-len,
						"0x%016" PRIX64 " + 0x%.3" PRIx32 ": %s",
						site.address, site.offset, site.name.c_str());
				} else if constexpr (W == 16) {
					len += snprintf(&buffer[len], sizeof(buffer)-len,
						"0x%016" PRIx64 " + 0x%.3" PRIx32 ": %s",
						(uint64_t)site.address, site.offset, site.name.c_str());
				}
				if (len > 0)
					print_function({buffer, (size_t)len});
				else
					print_function("Scuffed frame. Should not happen!");
			};
		if (ra) {
			print_trace(0, this->machine().cpu.pc());
			print_trace(1, this->machine().cpu.reg(REG_RA));
		} else {
			print_trace(-1, this->machine().cpu.pc());
		}
	}

	template <int W>
	void Memory<W>::protection_fault(address_t addr)
	{
		CPU<W>::trigger_exception(PROTECTION_FAULT, addr);
	}

	INSTANTIATE_32_IF_ENABLED(Memory);
	INSTANTIATE_64_IF_ENABLED(Memory);
	INSTANTIATE_128_IF_ENABLED(Memory);
}
