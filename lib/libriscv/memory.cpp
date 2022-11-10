#include "memory.hpp"

#include "machine.hpp"
#include "decoder_cache.hpp"
#include <inttypes.h>
#ifdef RISCV_BINARY_TRANSLATION
#include <dlfcn.h> // Linux-only
#endif

extern "C" char *
__cxa_demangle(const char *name, char *buf, size_t *n, int *status);

namespace riscv
{
	template <int W>
	Memory<W>::Memory(Machine<W>& mach, std::string_view bin,
					MachineOptions<W> options)
		: m_machine{mach},
#ifdef RISCV_FLAT_MEMORY
		  // Strategy: Allocate 32-byte aligned memory with 32 bytes
		  // extra at the end in order to avoid having to check
		  // address+sizeof(T) < memsize. Instead, it becomes:
		  // address < memsize, due to the 32 bytes over-allocation.
		  m_memdata {new (std::align_val_t(32)) uint8_t[options.memory_max + 32]},
		  m_memsize {options.memory_max},
#endif
		  m_original_machine {true},
		  m_binary {bin}
	{
#ifdef RISCV_FLAT_MEMORY
		if (UNLIKELY(this->m_memsize < bin.size()))
			throw MachineException(OUT_OF_MEMORY, "Out of memory", this->m_memsize);
		if (!m_binary.empty()) {
			// Load ELF binary into flat memory
			this->binary_loader(options);
		}
#else
		if (options.page_fault_handler != nullptr)
		{
			this->m_page_fault_handler = std::move(options.page_fault_handler);
		}
		else if (options.memory_max != 0)
		{
			const address_t pages_max = options.memory_max >> Page::SHIFT;
			assert(pages_max >= 1);
			this->m_page_fault_handler =
			[pages_max] (auto& mem, const address_t page, bool init) -> Page&
			{
				// create page on-demand
				if (mem.pages_active() < pages_max)
				{
					return mem.allocate_page(page,
						init ? PageData::INITIALIZED : PageData::UNINITIALIZED);
				}
				throw MachineException(OUT_OF_MEMORY, "Out of memory", pages_max);
			};
		} else {
			throw MachineException(OUT_OF_MEMORY, "Max memory was zero", 0);
		}
		if (!m_binary.empty()) {
			// Add a zero-page at the start of address space
			this->initial_paging();
			// load ELF binary into virtual memory
			this->binary_loader(options);
		}
#endif // RISCV_FLAT_MEMORY
	}
	template <int W>
	Memory<W>::Memory(Machine<W>& mach, const Machine<W>& other, MachineOptions<W> options)
	  : m_machine{mach},
#ifdef RISCV_FLAT_MEMORY
	    m_memdata{other.memory.m_memdata.get()},
		m_memsize{other.memory.m_memsize},
#endif
	    m_original_machine {false},
		m_binary{other.memory.binary()}
	{
		this->machine_loader(other, options);
	}

	template <int W>
	Memory<W>::~Memory()
	{
		this->clear_all_pages();
#ifdef RISCV_FLAT_MEMORY
		// only the original machine owns memdata range
		if (!this->m_original_machine) {
			m_memdata.release();
		}
#endif
#ifdef RISCV_RODATA_SEGMENT_IS_SHARED
		// only the original machine owns rodata range
		if (!this->m_original_machine) {
			m_ropages.pages.release();
		}
#endif
#ifdef RISCV_INSTR_CACHE
		delete[] m_decoder_cache;
#endif
#ifdef RISCV_BINARY_TRANSLATION
		if (m_bintr_dl)
			dlclose(m_bintr_dl);
#endif
	}

	template <int W> RISCV_INTERNAL
	void Memory<W>::reset()
	{
		// Hard to support because of things like
		// serialization, machine options and machine forks
	}

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

	template <int W> RISCV_INTERNAL
	void Memory<W>::binary_load_ph(const MachineOptions<W>& options, const Phdr* hdr)
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
		if (hdr->p_vaddr + len < hdr->p_vaddr) {
			throw MachineException(INVALID_PROGRAM, "Bogus ELF segment virtual base");
		}

		if (options.verbose_loader) {
		printf("* Loading program of size %zu from %p to virtual %p\n",
				len, src, (void*) (uintptr_t) hdr->p_vaddr);
		}
		// Serialize pages cannot be called with len == 0,
		// and there is nothing further to do.
		if (UNLIKELY(len == 0))
			return;

		// segment permissions
		const PageAttributes attr {
			 .read  = (hdr->p_flags & PF_R) != 0,
			 .write = (hdr->p_flags & PF_W) != 0,
			 .exec  = (hdr->p_flags & PF_X) != 0
		};
		if (options.verbose_loader) {
		printf("* Program segment readable: %d writable: %d  executable: %d\n",
				attr.read, attr.write, attr.exec);
		}

		if (attr.exec && machine().cpu.exec_seg_data() == nullptr)
		{
			constexpr address_t PMASK = Page::size()-1;
			const address_t pbase = (hdr->p_vaddr - 0x4) & ~PMASK;
			const size_t prelen  = hdr->p_vaddr - pbase;
			// The first 4 bytes is instruction alignment
			// The middle 4 bytes is the STOP instruction
			// The last 8 bytes is a relative jump (JR -4)
			const size_t midlen  = len + prelen + 12;
			const size_t plen = (midlen + PMASK) & ~PMASK;
			const size_t postlen = plen - midlen;
			//printf("Addr 0x%X Len %zx becomes 0x%X->0x%X PRE %zx MIDDLE %zu POST %zu TOTAL %zu\n",
			//	hdr->p_vaddr, len, pbase, pbase + plen, prelen, len, postlen, plen);
			if (UNLIKELY(prelen > plen || prelen + len > plen)) {
				throw MachineException(INVALID_PROGRAM, "Segment virtual base was bogus");
			}
			// An additional wrap-around check because we are adding 12 bytes
			// as well as additional padding to len.
			if (UNLIKELY(pbase + plen < pbase)) {
				throw MachineException(INVALID_PROGRAM, "Segment virtual base was bogus");
			}
			// Create the whole executable memory range
			m_exec_pagedata.reset(new uint8_t[plen]);
			m_exec_pagedata_size = plen;
			m_exec_pagedata_base = pbase;
			std::memset(&m_exec_pagedata[0],      0,   prelen);
			std::memcpy(&m_exec_pagedata[prelen], src, len);
			std::memset(&m_exec_pagedata[prelen + len], 0,   postlen);

			// Create a STOP instruction at the end of execute area
			// It is used by vmcall and preempt to stop after a function call
			address_t exit_lenalign = address_t(len + 0x3) & ~address_t(0x3);
			this->m_exit_address = hdr->p_vaddr + exit_lenalign;
			struct {
				// STOP
				const uint32_t stop_instr = 0x7ff00073;
				// JMP -4 (jump back to STOP)
				const uint32_t jr4_instr = 0xffdff06f;
			} instrdata;
			std::memcpy(&m_exec_pagedata[prelen + exit_lenalign], &instrdata, sizeof(instrdata));

			// This is what the CPU instruction fetcher will use
			// RISCV_INBOUND_JUMPS_ONLY requires us to add extra bytes at the beginning
			// The STOP function mentioned right above this requires us to add 12 bytes at the end
			// -4...0: Zero bytes that allow jumping to the start of exec before a pending increment
			// 0...len: The execute segment
			// len..+ 4: The STOP function
			auto* exec_offset = this->get_exec_segment(pbase);
			machine().cpu.initialize_exec_segs(exec_offset, hdr->p_vaddr-4, len + 8);
#if defined(RISCV_INSTR_CACHE)
			// + 8: A jump instruction that prevents crashes if someone
			// resumes the emulator after a STOP happened. It also helps
			// the debugger by not causing an exception, and will instead
			// loop back to the STOP instruction.
			// The instruction must be a part of the decoder cache.
			this->generate_decoder_cache(options, pbase, hdr->p_vaddr, len + 8);
#endif
			// Nothing more to do here, if execute-only
			if (!attr.read)
				return;
		} else if (attr.exec) {
#ifdef RISCV_INSTR_CACHE
			throw MachineException(INVALID_PROGRAM,
				"Binary can not have more than one executable segment!");
#endif
		}
		// We would normally never allow this
		if (attr.exec && attr.write) {
			if (!options.allow_write_exec_segment) {
				throw MachineException(INVALID_PROGRAM,
					"Insecure ELF has writable executable code");
			}
		}
		// In some cases we want to enforce execute-only
		if (attr.exec && (attr.read || attr.write)) {
			if (options.enforce_exec_only) {
				throw MachineException(INVALID_PROGRAM, "Execute segment must be execute-only");
			}
		}
#ifdef RISCV_FLAT_MEMORY
		fault_if_unreadable(hdr->p_vaddr, len);
		std::memcpy(&m_memdata[hdr->p_vaddr], src, len);
		return;
#else

# ifdef RISCV_RODATA_SEGMENT_IS_SHARED
		if (attr.read && !attr.write && m_ropages.end == 0) {
			// If the serialization fails, we will fallback to memcpy
			// with set_page_attr, like normal.
			if (serialize_pages(m_ropages, hdr->p_vaddr, src, len, attr))
				return;
		}
# endif

		// Load into virtual memory
		this->memcpy(hdr->p_vaddr, src, len);

		if (options.protect_segments) {
			this->set_page_attr(hdr->p_vaddr, len, attr);
		}
		else {
			// this might help execute simplistic barebones programs
			this->set_page_attr(hdr->p_vaddr, len, {
				 .read = true, .write = true, .exec = true
			});
		}
#endif
	}

	template <int W> RISCV_INTERNAL
	bool Memory<W>::serialize_pages(MemoryArea& area,
		address_t addr, const char* src, size_t size, PageAttributes attr)
	{
#ifdef RISCV_RODATA_SEGMENT_IS_SHARED
		static constexpr address_t PSIZEMASK = Page::size()-1;
		// It is not an optimization to store 1-2 pages
		if (size < 2*Page::size()) {
			area.pages = nullptr;
			area.begin = 0;
			area.end = 0;
			return false;
		}

		const address_t prebase = addr & ~PSIZEMASK;
		const address_t prelen  = addr - prebase;
		const address_t postbase = (addr + size) & ~PSIZEMASK;
		const address_t lastpage_len = (addr + size) - postbase;
		const address_t postlen = Page::size() - lastpage_len;
		// The total length should be a page-sized length
		const address_t total_len = prelen + size + postlen;
		assert((total_len & ~PSIZEMASK) == total_len);

		// Create the first and last page
		// TODO: Make this a PageData struct for alignment guarantees
		auto* pagedata = new uint8_t[Page::size() * 2] {};
		// Fill in the first page (at page 0)
		std::memset(&pagedata[0], 0,   prelen);
		std::memcpy(&pagedata[prelen], src,  Page::size() - prelen);
		// Fill in the last page (at page 1)
		std::memset(&pagedata[Page::size() + lastpage_len], 0,   postlen);
		std::memcpy(&pagedata[Page::size()], &src[postbase - addr], lastpage_len);
		area.data.reset(pagedata);

		const size_t npages = total_len / Page::size();
		area.pages.reset(new Page[npages]);
		// Create share-able range
		area.begin = addr / Page::size();
		area.end   = area.begin + npages;

		for (size_t i = 0; i < npages; i++) {
			area.pages[i].attr = attr;
			// None of the pages own their page memory
			area.pages[i].attr.non_owning = true;

			// We have custom page data for the first and last page
			if (i == 0 || i == npages-1) {
				const size_t offset = (i == 0) ? 0 : Page::size();
				area.pages[i].m_page.reset((PageData*) &pagedata[offset]);
			} else {
				const size_t offset = i * Page::size() - prelen;
				area.pages[i].m_page.reset((PageData*) &src[offset]);
			}
		}
#endif
		return true;
	}

	// ELF32 and ELF64 loader
	template <int W> RISCV_INTERNAL
	void Memory<W>::binary_loader(const MachineOptions<W>& options)
	{
		if (UNLIKELY(m_binary.size() < sizeof(Ehdr))) {
			throw MachineException(INVALID_PROGRAM, "ELF program too short");
		}
		const auto* elf = (Ehdr*) m_binary.data();
		if (UNLIKELY(!validate_header<Ehdr> (elf))) {
			throw MachineException(INVALID_PROGRAM, "Invalid ELF header! Mixup between 32- and 64-bit?", elf->e_ident[EI_CLASS]);
		}
		if (UNLIKELY(elf->e_type != ET_EXEC)) {
			throw MachineException(INVALID_PROGRAM, "ELF program is not an executable type. Trying to load a dynamic library?");
		}
		if (UNLIKELY(elf->e_machine != EM_RISCV)) {
			throw MachineException(INVALID_PROGRAM, "ELF program is not a RISC-V executable. Wrong architecture.");
		}

		// enumerate & load loadable segments
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
		if (UNLIKELY(elf->e_phoff + program_headers * sizeof(Phdr) > m_binary.size())) {
			throw MachineException(INVALID_PROGRAM, "ELF program-headers are outside the binary");
		}

		const auto* phdr = (Phdr*) (m_binary.data() + elf->e_phoff);
		this->m_start_address = elf->e_entry;
		this->m_heap_address = 0;

		for (const auto* hdr = phdr; hdr < phdr + program_headers; hdr++)
		{
			// Detect overlapping segments
			for (const auto* ph = phdr; ph < hdr; ph++) {
				if (hdr->p_type == PT_LOAD && ph->p_type == PT_LOAD)
				if (ph->p_vaddr < hdr->p_vaddr + hdr->p_filesz &&
					ph->p_vaddr + ph->p_filesz >= hdr->p_vaddr) {
					// Normally we would not care, but no normal ELF
					// has overlapping segments, so treat as bogus.
					throw MachineException(INVALID_PROGRAM, "Overlapping ELF segments");
				}
			}

			switch (hdr->p_type)
			{
				case PT_LOAD:
					// loadable program segments
					if (options.load_program) {
						binary_load_ph(options, hdr);
					}
					break;
				case PT_GNU_STACK:
					// This seems to be a mark for executable stack. Big NO!
					break;
				case PT_GNU_RELRO:
					//throw std::runtime_error(
					//	"Dynamically linked ELF binaries are not supported");
					break;
			}

			address_t endm = hdr->p_vaddr + hdr->p_memsz;
			endm += Page::size()-1; endm &= ~address_t(Page::size()-1);
			if (this->m_heap_address < endm)
				this->m_heap_address = endm;
		}
		// The base mmap address starts at heap start + BRK_MAX
		// TODO: We should check if the heap starts too close to the end
		// of the address space now, and move it around if necessary.
		this->m_mmap_address = m_heap_address + BRK_MAX;

		// Default stack
		this->m_stack_address = mmap_allocate(options.stack_size) + options.stack_size;

		//this->relocate_section(".rela.dyn", ".symtab");

		if (options.verbose_loader) {
		printf("* Entry is at %p\n", (void*) (uintptr_t) this->start_address());
		}
	}

	template <int W> RISCV_INTERNAL
	void Memory<W>::machine_loader(
		const Machine<W>& master, const MachineOptions<W>& options)
	{
		// Some machines don't need custom PF handlers
		this->m_page_fault_handler = master.memory.m_page_fault_handler;

		if (options.minimal_fork == false)
		{
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
				m_pages.emplace(std::piecewise_construct,
					std::forward_as_tuple(it.first),
					std::forward_as_tuple(attr, (PageData*) page.data())
				);
			}
		}
		this->m_start_address = master.memory.m_start_address;
		this->m_stack_address = master.memory.m_stack_address;
		this->m_exit_address = master.memory.m_exit_address;
		this->m_heap_address = master.memory.m_heap_address;
		this->m_mmap_address = master.memory.m_mmap_address;

		// base address, size and PC-relative data pointer for instructions
		this->m_exec_pagedata_base = master.memory.m_exec_pagedata_base;
		this->m_exec_pagedata_size = master.memory.m_exec_pagedata_size;
#ifdef RISCV_INSTR_CACHE
		this->m_exec_decoder = master.memory.m_exec_decoder;
#endif

#ifdef RISCV_RODATA_SEGMENT_IS_SHARED
		this->m_ropages.begin = master.memory.m_ropages.begin;
		this->m_ropages.end   = master.memory.m_ropages.end;
		this->m_ropages.pages.reset(master.memory.m_ropages.pages.get());
#endif
		// invalidate all cached pages, because references are invalidated
		this->invalidate_reset_cache();
	}

	template <int W>
	const typename Memory<W>::Shdr* Memory<W>::section_by_name(const char* name) const
	{
		const auto* shdr = elf_offset<Shdr> (elf_header()->e_shoff);
		const auto& shstrtab = shdr[elf_header()->e_shstrndx];
		const char* strings = elf_offset<char>(shstrtab.sh_offset);

		for (auto i = 0; i < elf_header()->e_shnum; i++)
		{
			const char* shname = &strings[shdr[i].sh_name];
			if (strcmp(shname, name) == 0) {
				return &shdr[i];
			}
		}
		return nullptr;
	}

	template <int W>
	const typename Elf<W>::Sym* Memory<W>::resolve_symbol(const char* name) const
	{
		if (UNLIKELY(m_binary.empty())) return nullptr;
		const auto* sym_hdr = section_by_name(".symtab");
		if (UNLIKELY(sym_hdr == nullptr)) return nullptr;
		const auto* str_hdr = section_by_name(".strtab");
		if (UNLIKELY(str_hdr == nullptr)) return nullptr;

		const auto* symtab = elf_sym_index(sym_hdr, 0);
		const size_t symtab_ents = sym_hdr->sh_size / sizeof(typename Elf<W>::Sym);
		const char* strtab = elf_offset<char>(str_hdr->sh_offset);

		for (size_t i = 0; i < symtab_ents; i++)
		{
			const char* symname = &strtab[symtab[i].st_name];
			if (strcmp(symname, name) == 0) {
				return &symtab[i];
			}
		}
		return nullptr;
	}


	template <typename Sym>
	static void elf_print_sym(const Sym* sym)
	{
		if constexpr (sizeof(Sym::st_value) == 4) {
			printf("-> Sym is at 0x%" PRIX32 " with size %" PRIu32 ", type %u name %u\n",
				sym->st_value, sym->st_size,
				ELF32_ST_TYPE(sym->st_info), sym->st_name);
		} else {
			printf("-> Sym is at 0x%" PRIX64 " with size %" PRIu64 ", type %u name %u\n",
				(uint64_t)sym->st_value, sym->st_size,
				ELF64_ST_TYPE(sym->st_info), sym->st_name);
		}
	}

	template <int W> RISCV_INTERNAL
	void Memory<W>::relocate_section(const char* section_name, const char* sym_section)
	{
		const auto* rela = section_by_name(section_name);
		if (rela == nullptr) return;
		const auto* dyn_hdr = section_by_name(sym_section);
		if (dyn_hdr == nullptr) return;
		const size_t rela_ents = rela->sh_size / sizeof(Elf32_Rela);

		auto* rela_addr = elf_offset<Elf32_Rela>(rela->sh_offset);
		for (size_t i = 0; i < rela_ents; i++)
		{
			const uint32_t symidx = ELF32_R_SYM(rela_addr[i].r_info);
			auto* sym = elf_sym_index(dyn_hdr, symidx);

			const uint8_t type = ELF32_ST_TYPE(sym->st_info);
			if (type == STT_FUNC || type == STT_OBJECT)
			{
				auto* entry = elf_offset<address_t> (rela_addr[i].r_offset);
				auto* final = elf_offset<address_t> (sym->st_value);
				if constexpr (true)
				{
					//printf("Relocating rela %zu with sym idx %u where 0x%X -> 0x%X\n",
					//		i, symidx, rela_addr[i].r_offset, sym->st_value);
					elf_print_sym<typename Elf<W>::Sym>(sym);
				}
				*(address_t*) entry = (address_t) (uintptr_t) final;
			}
		}
	}

	template <int W>
	std::string Memory<W>::get_page_info(address_t addr) const
	{
		char buffer[1024];
		int len;
		if constexpr (W == 4) {
			len = snprintf(buffer, sizeof(buffer),
				"[0x%08" PRIX32 "] %s", addr, get_page(addr).to_string().c_str());
		} else if constexpr (W == 8) {
			len = snprintf(buffer, sizeof(buffer),
				"[0x%016" PRIX64 "] %s", addr, get_page(addr).to_string().c_str());
		} else if constexpr (W == 16) {
			len = snprintf(buffer, sizeof(buffer),
				"[0x%016" PRIX64 "] %s", (uint64_t)addr, get_page(addr).to_string().c_str());
		}
		return std::string(buffer, len);
	}

	template <int W>
	typename Memory<W>::Callsite Memory<W>::lookup(address_t address) const
	{
		const auto* sym_hdr = section_by_name(".symtab");
		if (sym_hdr == nullptr) return {};
		const auto* str_hdr = section_by_name(".strtab");
		if (str_hdr == nullptr) return {};
		// backtrace can sometimes find null addresses
		if (address == 0x0) return {};

		const auto* symtab = elf_sym_index(sym_hdr, 0);
		const size_t symtab_ents = sym_hdr->sh_size / sizeof(typename Elf<W>::Sym);
		const char* strtab = elf_offset<char>(str_hdr->sh_offset);

		const auto result =
			[] (const char* strtab, address_t addr, const auto* sym)
		{
			const char* symname = &strtab[sym->st_name];
			char* dma = __cxa_demangle(symname, nullptr, nullptr, nullptr);
			return Callsite {
				.name = (dma) ? dma : symname,
				.address = sym->st_value,
				.offset = (uint32_t) (addr - sym->st_value),
				.size   = sym->st_size
			};
		};

		const typename Elf<W>::Sym* best = nullptr;
		for (size_t i = 0; i < symtab_ents; i++)
		{
			if (ELF32_ST_TYPE(symtab[i].st_info) != STT_FUNC) continue;
			/*printf("Testing %#X vs  %#X to %#X = %s\n",
					address, symtab[i].st_value,
					symtab[i].st_value + symtab[i].st_size, symname);*/

			if (address >= symtab[i].st_value &&
				address < symtab[i].st_value + symtab[i].st_size)
			{
				// exact match
				return result(strtab, address, &symtab[i]);
			}
			else if (address > symtab[i].st_value)
			{
				// best guess (symbol + 0xOff)
				best = &symtab[i];
			}
		}
		if (best)
			return result(strtab, address, best);
		return {};
	}
	template <int W>
	void Memory<W>::print_backtrace(
		std::function<void(std::string_view)> print_function)
	{
		auto print_trace =
			[this, print_function] (const int N, const address_type<W> addr) {
				// get information about the callsite
				const auto site = this->lookup(addr);
				// write information directly to stdout
				char buffer[8192];
				int len;
				if constexpr (W == 4) {
					len = snprintf(buffer, sizeof(buffer),
						"[%d] 0x%08" PRIx32 " + 0x%.3" PRIx32 ": %s",
						N, site.address, site.offset, site.name.c_str());
				} else if constexpr (W == 8) {
					len = snprintf(buffer, sizeof(buffer),
						"[%d] 0x%016" PRIX64 " + 0x%.3" PRIx32 ": %s",
						N, site.address, site.offset, site.name.c_str());
				} else if constexpr (W == 16) {
					len = snprintf(buffer, sizeof(buffer),
						"[%d] 0x%016" PRIx64 " + 0x%.3" PRIx32 ": %s",
						N, (uint64_t)site.address, site.offset, site.name.c_str());
				}
				if (len > 0)
					print_function({buffer, (size_t)len});
				else
					print_function("Scuffed frame. Should not happen!");
			};
		print_trace(0, this->machine().cpu.pc());
		print_trace(1, this->machine().cpu.reg(REG_RA));
	}

	template <int W>
	void Memory<W>::protection_fault(address_t addr)
	{
		CPU<W>::trigger_exception(PROTECTION_FAULT, addr);
		__builtin_unreachable();
	}

	template struct Memory<4>;
	template struct Memory<8>;
	template struct Memory<16>;
}
