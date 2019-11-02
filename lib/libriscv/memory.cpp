#include "memory.hpp"
#include "machine.hpp"
#include "elf.hpp"

namespace riscv
{
	template <int W>
	Memory<W>::Memory(Machine<W>& machine, std::vector<uint8_t> binary, bool protect)
		: m_machine{machine}, m_binary{std::move(binary)}
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
		this->binary_loader();
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

	// ELF32 version
	template <int W>
	void Memory<W>::binary_loader()
	{
		if (m_binary.size() < 64) {
			throw std::runtime_error("ELF binary too short");
		}
		// basic 32-bit ELF loader
		const auto* elf = (Ehdr*) m_binary.data();
		if (UNLIKELY(!validate_header<Ehdr> (elf))) {
			throw std::runtime_error("Invalid ELF header");
		}

		// enumerate & load loadable segments
		const auto* phdr = (Phdr*) (m_binary.data() + elf->e_phoff);
		const auto program_headers = elf->e_phnum;
		if (m_binary.size() < elf->e_phoff + program_headers * sizeof(Phdr)) {
			throw std::runtime_error("No room for ELF program-headers");
		}

		const auto program_begin = phdr->p_vaddr;
		auto program_end = program_begin;

		int seg = 0;
		for (const auto* hdr = phdr; hdr < phdr + program_headers; hdr++)
		{
			switch (hdr->p_type)
			{
				case PT_LOAD:
					binary_load_ph(hdr);
					seg++;
					break;
			}
		}

		// install .text and other ELF sections
		/*
		const auto* shdr = (Elf32_Shdr*) (m_binary.data() + elf->e_shoff);
		seg = 0;
		for (const auto* hdr = shdr; hdr < shdr + elf->e_shnum; hdr++)
		{
			if (hdr->sh_flags & SHF_ALLOC) {
				const bool writable   = hdr->sh_flags & SHF_WRITE;
				const bool executable = hdr->sh_flags & SHF_EXECINSTR;

				const auto*  src = m_binary.data() + hdr->sh_offset;
				const size_t len = hdr->sh_size;
				const address_t dst = hdr->sh_addr;

				printf("Loading section %d of size %zu from %p to virtual %p\n",
						seg, len, src, (void*) (uintptr_t) dst);
				printf("Section writable: %d  executable: %d\n", writable, executable);
				// load into virtual memory
				this->memcpy(dst, src, len);
				this->set_page_attr(dst, len, { .write = writable, .exec = executable });
			}
			seg++;
		}
		*/

		this->m_start_address = elf->e_entry;
		this->m_stack_address = program_begin;
		if (riscv::verbose_machine) {
		printf("* Entry is at %p\n", (void*) (uintptr_t) this->start_address());
		}
	}

	template <int W>
	void Memory<W>::protection_fault()
	{
		this->machine().cpu.trigger_interrupt(PROTECTION_FAULT);
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
		throw std::runtime_error("Out of memory");
	}

	const Page& Page::cow_page() noexcept {
		static Page zeroed_page;
		return zeroed_page; // read-only, zeroed page
	}

	template class Memory<4>;
}
