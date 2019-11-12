#include "memory.hpp"
#include "machine.hpp"
#include "elf.hpp"

namespace riscv
{
	template <int W>
	Memory<W>::Memory(Machine<W>& mach, const std::vector<uint8_t>& bin, bool protect)
		: m_machine{mach}, m_binary{bin}
	{
		this->m_protect_segments = protect;
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
		zp.attr.read = false;
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
		auto program_end = program_begin;
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

		if (riscv::verbose_machine) {
		printf("* Entry is at %p\n", (void*) (uintptr_t) this->start_address());
		}
	}

	template <int W>
	const typename Memory<W>::Shdr* Memory<W>::section_by_name(const char* name) const
	{
		const auto* shdr = elf_offset<Shdr> (elf_header()->e_shoff);
		const auto& shstrtab = shdr[elf_header()->e_shnum-1];
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
		assert(sym_hdr != nullptr);
		const auto* str_hdr = section_by_name(".strtab");
		assert(str_hdr != nullptr);

		const auto* symtab = elf_sym_index(sym_hdr, 0);
		const size_t symtab_ents = sym_hdr->sh_size / sizeof(typename Elf<W>::Sym);
		const char* strtab = elf_offset<char>(str_hdr->sh_offset);

		for (size_t i = 0; i < symtab_ents; i++)
		{
			const char* symname = &strtab[symtab[i].st_name];
			//printf("Testing %s vs %s\n", symname, name);
			if (strcmp(symname, name) == 0) {
				return &symtab[i];
			}
		}
		return nullptr;
	}

	template <int W>
	address_type<W> Memory<W>::resolve_address(const std::string& name)
	{
		auto* sym = resolve_symbol(name.c_str());
		if (sym) return sym->st_value;
		return 0x0;
	}

	template <int W>
	Page& Memory<W>::default_page_fault(Memory<W>& mem, const size_t page)
	{
		// create page on-demand
		if (mem.active_pages() < mem.total_pages())
		{
			auto it = mem.pages().emplace(
				std::piecewise_construct,
				std::forward_as_tuple(page),
				std::forward_as_tuple());
			return it.first->second;
		}
		throw MachineException("Out of memory");
	}

	const Page& Page::cow_page() noexcept {
		static Page zeroed_page;
		return zeroed_page; // read-only, zeroed page
	}

	template class Memory<4>;
}
