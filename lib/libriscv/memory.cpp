#include "memory.hpp"

#include "machine.hpp"
#include "decoder_cache.hpp"
#include <stdexcept>
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
		  m_original_machine {true},
		  m_binary{bin}
	{
		if (options.page_fault_handler != nullptr)
		{
			this->m_page_fault_handler = std::move(options.page_fault_handler);
		}
		else if (options.memory_max != 0)
		{
			const address_t pages_max = options.memory_max >> Page::SHIFT;
			assert(pages_max >= 1);
			this->m_page_fault_handler =
				[pages_max] (auto& mem, const address_t page) -> Page&
				{
					// create page on-demand
					if (mem.pages_active() < pages_max)
					{
						return mem.allocate_page(page);
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
	}
	template <int W>
	Memory<W>::Memory(Machine<W>& mach, const Machine<W>& other, MachineOptions<W> options)
	  : m_machine{mach},
	    m_original_machine {false},
		m_binary{other.memory.binary()}
	{
		this->machine_loader(other, options);
	}

	template <int W>
	Memory<W>::~Memory()
	{
		this->clear_all_pages();
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

	template <int W>
	void Memory<W>::reset()
	{
		// Hard to support because of things like
		// serialization, machine options and machine forks
	}

	template <int W>
	void Memory<W>::clear_all_pages()
	{
		this->m_pages.clear();
		this->m_rd_cache = {};
		this->m_wr_cache = {};
	}

	template <int W>
	void Memory<W>::initial_paging()
	{
		if (m_pages.find(0) == m_pages.end()) {
			// add a guard page to catch zero-page accesses
			install_shared_page(0, Page::guard_page());
		}
	}

	template <int W>
	void Memory<W>::binary_load_ph(const MachineOptions<W>& options, const Phdr* hdr)
	{
		const auto*  src = m_binary.data() + hdr->p_offset;
		const size_t len = hdr->p_filesz;
		if (m_binary.size() <= hdr->p_offset ||
			hdr->p_offset + len < hdr->p_offset)
		{
			throw std::runtime_error("Bogus ELF program segment offset");
		}
		if (m_binary.size() < hdr->p_offset + len) {
			throw std::runtime_error("Not enough room for ELF program segment");
		}
		if (hdr->p_vaddr + len < hdr->p_vaddr) {
			throw std::runtime_error("Bogus ELF segment virtual base");
		}

		if (options.verbose_loader) {
		printf("* Loading program of size %zu from %p to virtual %p\n",
				len, src, (void*) (uintptr_t) hdr->p_vaddr);
		}
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
			const address_t pbase = (hdr->p_vaddr - 0x4) & ~(address_t) PMASK;
			const size_t prelen  = hdr->p_vaddr - pbase;
			// The first 4 bytes is instruction alignment
			// The middle 4 bytes is the STOP instruction
			// The last 8 bytes is a relative jump (JR -4)
			const size_t midlen  = len + prelen + 12;
			const size_t plen =
				(PMASK & midlen) ? ((midlen + Page::size()) & ~PMASK) : midlen;
			const size_t postlen = plen - midlen;
			//printf("Addr 0x%X Len %zx becomes 0x%X->0x%X PRE %zx MIDDLE %zu POST %zu TOTAL %zu\n",
			//	hdr->p_vaddr, len, pbase, pbase + plen, prelen, len, postlen, plen);
			if (UNLIKELY(prelen > plen || prelen + len > plen)) {
				throw std::runtime_error("Segment virtual base was bogus");
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
			// 0...len: The regular execute segment
			// len..+ 4: The STOP function
			auto* exec_offset = m_exec_pagedata.get() - pbase;
			machine().cpu.initialize_exec_segs(exec_offset, hdr->p_vaddr, len + 4);
#if defined(RISCV_INSTR_CACHE)
			// + 8: A jump instruction that prevents crashes if someone
			// resumes the emulator after a STOP happened. It also helps
			// the debugger by not causing an exception, and will instead
			// loop back to the STOP instruction.
			// The instruction must be a part of the decoder cache.
			this->generate_decoder_cache(options, pbase, hdr->p_vaddr, len + 8);
#endif
			(void) options;
			// Nothing more to do here, if execute-only
			if (!attr.read)
				return;
		} else if (attr.exec) {
#ifdef RISCV_INSTR_CACHE
			throw std::runtime_error(
				"Binary can not have more than one executable segment!");
#endif
		}
		// We would normally never allow this
		if (attr.exec && attr.write) {
			if (options.allow_write_exec_segment == false) {
				throw std::runtime_error(
					"Insecure ELF has writable executable code");
			}
		}

#ifdef RISCV_RODATA_SEGMENT_IS_SHARED
		if (attr.read && !attr.write && m_ropages.end == 0) {
			serialize_pages(m_ropages, hdr->p_vaddr, src, len, attr);
			return;
		}
#endif

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
	}

	template <int W>
	void Memory<W>::serialize_pages(MemoryArea& area,
		address_t addr, const char* src, size_t size, PageAttributes attr)
	{
#ifdef RISCV_RODATA_SEGMENT_IS_SHARED
		constexpr address_t PSIZEMASK = Page::size()-1;
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
	}

	// ELF32 and ELF64 loader
	template <int W>
	void Memory<W>::binary_loader(const MachineOptions<W>& options)
	{
		if (UNLIKELY(m_binary.size() < sizeof(Ehdr))) {
			throw std::runtime_error("ELF binary too short");
		}
		const auto* elf = (Ehdr*) m_binary.data();
		if (UNLIKELY(!validate_header<Ehdr> (elf))) {
			throw std::runtime_error("Invalid ELF header! Mixup between 32- and 64-bit?");
		}

		// enumerate & load loadable segments
		const auto program_headers = elf->e_phnum;
		if (UNLIKELY(program_headers <= 0)) {
			throw std::runtime_error("ELF with no program-headers");
		}
		if (UNLIKELY(program_headers >= 10)) {
			throw std::runtime_error("ELF with too many program-headers");
		}
		if (UNLIKELY(elf->e_phoff > 0x4000)) {
			throw std::runtime_error("ELF program-headers have bogus offset");
		}
		if (UNLIKELY(elf->e_phoff + program_headers * sizeof(Phdr) > m_binary.size())) {
			throw std::runtime_error("ELF program-headers are outside the binary");
		}

		const auto* phdr = (Phdr*) (m_binary.data() + elf->e_phoff);
		const auto program_begin = phdr->p_vaddr;
		this->m_start_address = elf->e_entry;
		this->m_stack_address = program_begin;

		int seg = 0;
		for (const auto* hdr = phdr; hdr < phdr + program_headers; hdr++)
		{
			// Detect overlapping segments
			for (const auto* ph = phdr; ph < hdr; ph++) {
				if (hdr->p_type == PT_LOAD && ph->p_type == PT_LOAD)
				if (ph->p_vaddr < hdr->p_vaddr + hdr->p_filesz &&
					ph->p_vaddr + ph->p_filesz >= hdr->p_vaddr) {
					// Normally we would not care, but no normal ELF
					// has overlapping segments, so treat as bogus.
					throw std::runtime_error("Overlapping ELF segments");
				}
			}

			switch (hdr->p_type)
			{
				case PT_LOAD:
					// loadable program segments
					if (options.load_program) {
						binary_load_ph(options, hdr);
					}
					seg++;
					break;
				case PT_GNU_STACK:
					//printf("GNU_STACK: 0x%X\n", hdr->p_vaddr);
					this->m_stack_address = hdr->p_vaddr; // ??
					break;
				case PT_GNU_RELRO:
					//throw std::runtime_error(
					//	"Dynamically linked ELF binaries are not supported");
					break;
			}
		}

		//this->relocate_section(".rela.dyn", ".symtab");

		// NOTE: if the stack is very low, some stack pointer value could
		// become 0x0 which could alter the behavior of the program,
		// even though the address might be legitimate. To solve this, we move
		// the stack at that time to a safer location.
		if (this->m_stack_address < 0x100000) {
			this->m_stack_address = 0x40000000;
		}

		if (options.verbose_loader) {
		printf("* Entry is at %p\n", (void*) (uintptr_t) this->start_address());
		}
	}

	template <int W>
	void Memory<W>::machine_loader(
		const Machine<W>& master, const MachineOptions<W>&)
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
			m_pages.emplace(std::piecewise_construct,
				std::forward_as_tuple(it.first),
				std::forward_as_tuple(attr, (PageData*) page.data())
			);
		}
		this->m_start_address = master.memory.m_start_address;
		this->m_stack_address = master.memory.m_stack_address;
		this->m_exit_address = master.memory.m_exit_address;
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
			printf("-> Sym is at 0x%X with size %u, type %u name %u\n",
				sym->st_value, sym->st_size,
				ELF32_ST_TYPE(sym->st_info), sym->st_name);
		} else {
			printf("-> Sym is at 0x%lX with size %lu, type %u name %u\n",
				(long)sym->st_value, sym->st_size,
				ELF64_ST_TYPE(sym->st_info), sym->st_name);
		}
	}

	template <int W>
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
				"[0x%08X] %s", addr, get_page(addr).to_string().c_str());
		} else if constexpr (W == 8) {
			len = snprintf(buffer, sizeof(buffer),
				"[0x%016lX] %s", addr, get_page(addr).to_string().c_str());
		} else if constexpr (W == 16) {
			len = snprintf(buffer, sizeof(buffer),
				"[0x%016lX] %s", (uint64_t)addr, get_page(addr).to_string().c_str());
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
	void Memory<W>::print_backtrace(void(*print_function)(std::string_view))
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
						"[%d] 0x%08x + 0x%.3x: %s",
						N, site.address, site.offset, site.name.c_str());
				} else if constexpr (W == 8) {
					len = snprintf(buffer, sizeof(buffer),
						"[%d] 0x%016lx + 0x%.3x: %s",
						N, site.address, site.offset, site.name.c_str());
				} else if constexpr (W == 16) {
					len = snprintf(buffer, sizeof(buffer),
						"[%d] 0x%016lx + 0x%.3x: %s",
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
