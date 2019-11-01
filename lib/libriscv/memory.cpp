#include "memory.hpp"
#include "machine.hpp"
#include "util/elf.h"

namespace riscv
{
	template <typename Class>
	inline bool validate_header(const Class* hdr)
	{
		return	hdr->e_ident[0] == 0x7F &&
				hdr->e_ident[1] == 'E'  &&
				hdr->e_ident[2] == 'L'  &&
				hdr->e_ident[3] == 'F';
	}

	template <int W>
	Memory<W>::Memory(Machine<W>& machine, std::vector<uint8_t> binary)
		: m_machine{machine}, m_binary{std::move(binary)}
	{
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

	// ELF32 version
	template <>
	void Memory<4>::binary_loader()
	{
		// basic 32-bit ELF loader
		const auto* elf = (Elf32_Ehdr*) m_binary.data();
		assert(validate_header<Elf32_Ehdr> (elf));

		// enumerate & load loadable segments
		const auto* phdr = (Elf32_Phdr*) (m_binary.data() + elf->e_phoff);
		const uint32_t program_headers = elf->e_phnum;
		const auto program_begin = phdr->p_vaddr;
		uint32_t program_end = program_begin;

		int seg = 0;
		for (const auto* hdr = phdr; hdr < phdr + program_headers; hdr++)
		{
			if (hdr->p_type != PT_LOAD) continue;
			const auto*  src = m_binary.data() + hdr->p_offset;
			const size_t len = hdr->p_filesz;
			if (machine().verbose_machine) {
			printf("* Loading program %d of size %zu from %p to virtual %p\n",
					seg, len, src, (void*) (uintptr_t) hdr->p_vaddr);
			}
			// load into virtual memory
			this->memcpy(hdr->p_vaddr, src, len);
			// set permissions
			const bool readable   = hdr->p_flags & PF_R;
			const bool writable   = hdr->p_flags & PF_W;
			const bool executable = hdr->p_flags & PF_X;
			if (machine().verbose_machine) {
			printf("* Program segment readable: %d writable: %d  executable: %d\n",
					readable, writable, executable);
			}
			this->set_page_attr(hdr->p_vaddr, len, {
				 .read = readable, .write = writable, .exec = executable
			});
			seg++;
			// find program end
			program_end = std::max(program_end, (uint32_t) (hdr->p_vaddr + len));
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
		this->m_elf_end_vaddr = program_end;
		if (machine().verbose_machine) {
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
