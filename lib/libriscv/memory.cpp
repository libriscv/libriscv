#include "memory.hpp"
#include "machine.hpp"
#include "elf.hpp"

namespace riscv
{
	template <int W>
	Memory<W>::Memory(Machine<W>& mach, const std::vector<uint8_t>& bin, address_t max_mem)
		: m_machine{mach}, m_binary{bin}, m_protect_segments {true}
	{
		assert(max_mem % Page::size() == 0);
		assert(max_mem >= Page::size());
		this->m_pages_total = max_mem / Page::size();
		this->reset();
	}

	template <int W>
	void Memory<W>::reset()
	{
		// initialize paging (which clears all pages) before loading binary
		this->initial_paging();
		// load ELF binary into virtual memory
		if (!m_binary.empty()) this->binary_loader();
	}

	template <int W>
	void Memory<W>::initial_paging()
	{
		this->m_pages.clear();
		// make the zero-page unreadable (to trigger faults on null-pointer accesses)
		auto& zp = this->create_page(0);
		zp.attr = { .read = false, .write = false, .exec = false };
	}

	template <int W>
	void Memory<W>::binary_load_ph(const Phdr* hdr)
	{
		const auto*  src = m_binary.data() + hdr->p_offset;
		const size_t len = hdr->p_filesz;
		if (m_binary.size() < hdr->p_offset + len) {
			throw std::runtime_error("Not enough room for ELF program segment");
		}

		if (riscv::verbose_machine) {
		printf("* Loading program of size %zu from %p to virtual %p\n",
				len, src, (void*) (uintptr_t) hdr->p_vaddr);
		}
		// load into virtual memory
		this->memcpy(hdr->p_vaddr, src, len);
		// set permissions
		const bool readable   = hdr->p_flags & PF_R;
		const bool writable   = hdr->p_flags & PF_W;
		const bool executable = hdr->p_flags & PF_X;
		if (riscv::verbose_machine) {
		printf("* Program segment readable: %d writable: %d  executable: %d\n",
				readable, writable, executable);
		}
		if (this->m_protect_segments) {
			this->set_page_attr(hdr->p_vaddr, len, {
				 .read = readable, .write = writable, .exec = executable
			});
		}
		else {
			// this might help execute simplistic barebones programs
			this->set_page_attr(hdr->p_vaddr, len, {
				 .read = true, .write = true, .exec = true
			});
		}
		// find program end
		m_elf_end_vaddr = std::max(m_elf_end_vaddr, (uint32_t) (hdr->p_vaddr + len));
		// set the default exit function address for vm calls
		this->m_exit_address = resolve_address("_exit");
	}

	// ELF32 and ELF64 loader
	template <int W>
	void Memory<W>::binary_loader()
	{
		if (UNLIKELY(m_binary.size() < 64)) {
			throw std::runtime_error("ELF binary too short");
		}
		const auto* elf = (Ehdr*) m_binary.data();
		if (UNLIKELY(!validate_header<Ehdr> (elf))) {
			throw std::runtime_error("Invalid ELF header");
		}

		// enumerate & load loadable segments
		const auto* phdr = (Phdr*) (m_binary.data() + elf->e_phoff);
		const auto program_headers = elf->e_phnum;
		if (UNLIKELY(program_headers <= 0)) {
			throw std::runtime_error("ELF with no program-headers");
		}
		if (UNLIKELY(m_binary.size() < elf->e_phoff + program_headers * sizeof(Phdr))) {
			throw std::runtime_error("No room for ELF program-headers");
		}

		const auto program_begin = phdr->p_vaddr;
		this->m_start_address = elf->e_entry;
		this->m_stack_address = program_begin;

		int seg = 0;
		for (const auto* hdr = phdr; hdr < phdr + program_headers; hdr++)
		{
			switch (hdr->p_type)
			{
				case PT_LOAD:
					binary_load_ph(hdr);
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

		if (riscv::verbose_machine) {
		printf("* Entry is at %p\n", (void*) (uintptr_t) this->start_address());
		}
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
	const typename Elf<W>::Sym* Memory<W>::resolve_symbol(const char* name)
	{
		const auto* sym_hdr = section_by_name(".symtab");
		if (sym_hdr == nullptr) return nullptr;
		const auto* str_hdr = section_by_name(".strtab");
		if (str_hdr == nullptr) return nullptr;

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
		printf("-> Sym is at 0x%X with size %u, type %u name %u\n",
				sym->st_value, sym->st_size,
				ELF32_ST_TYPE(sym->st_info), sym->st_name);
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
					printf("Relocating rela %zu with sym idx %u where 0x%X -> 0x%X\n",
							i, symidx, rela_addr[i].r_offset, sym->st_value);
					elf_print_sym<typename Elf<W>::Sym>(sym);
				}
				*(address_t*) entry = (address_t) (uintptr_t) final;
			}
		}
	}

	template <int W>
	Page& Memory<W>::allocate_page(const size_t page)
	{
		const auto& it = pages().emplace(page, Page{});
		m_pages_highest = std::max(m_pages_highest, pages().size());
		// if this page was read-cached, invalidate it
		this->invalidate_page(page, it.first->second);
		// return new page
		return it.first->second;
	}

	template <int W>
	Page& Memory<W>::default_page_fault(Memory<W>& mem, const size_t page)
	{
		// create page on-demand
		if (mem.pages_active() < mem.pages_total())
		{
			return mem.allocate_page(page);
		}
		throw MachineException(OUT_OF_MEMORY, "Out of memory");
	}

	inline static Page create_cow() {
		Page page;
		page.attr = {
			.read   = true,
			.write  = false,
			.exec   = false,
			.is_cow = true
		};
		return page;
	}
	const Page& Page::cow_page() noexcept {
		static Page zeroed_page = create_cow();
		return zeroed_page; // read-only, zeroed page
	}

	template struct Memory<4>;
}

void* operator new[](size_t size, const char*, int, unsigned, const char*, int)
{
	return ::operator new[] (size);
}
void* operator new[](size_t size, size_t, size_t, const char*, int, unsigned, const char*, int)
{
	return ::operator new[](size);
}
