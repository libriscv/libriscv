#include "memory.hpp"
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

	template <>
	Memory<4>::Memory(Machine<4>& machine, std::vector<uint8_t> binary)
		: m_machine{machine}
	{
		// basic 32-bit ELF loader
		const auto* elf = (Elf32_Ehdr*) binary.data();
		assert(validate_header<Elf32_Ehdr> (elf));

		// enumerate & load loadable segments
		const auto* phdr = (Elf32_Phdr*) (binary.data() + elf->e_phoff);
		const uint32_t program_headers = elf->e_phnum;
		const auto program_begin = phdr->p_vaddr;

		int seg = 0;
		for (const auto* hdr = phdr; hdr < phdr + program_headers; hdr++)
		{
			const auto*  src = binary.data() + hdr->p_offset;
			const size_t len = hdr->p_filesz;
			printf("* Loading program %d of size %zu from %p to virtual %p\n",
					seg, len, src, (void*) (uintptr_t) hdr->p_vaddr);
			// load into virtual memory
			this->memcpy(hdr->p_vaddr, src, len);
			// set permissions
			const bool readable   = hdr->p_flags & PF_R;
			const bool writable   = hdr->p_flags & PF_W;
			const bool executable = hdr->p_flags & PF_X;
			printf("* Program segment readable: %d writable: %d  executable: %d\n",
					readable, writable, executable);
			this->set_page_attr(hdr->p_vaddr, len, {
				 .read = readable, .write = writable, .exec = executable
			});
			seg++;
		}

		// install .text and other ELF sections
		/*
		const auto* shdr = (Elf32_Shdr*) (binary.data() + elf->e_shoff);
		seg = 0;
		for (const auto* hdr = shdr; hdr < shdr + elf->e_shnum; hdr++)
		{
			if (hdr->sh_flags & SHF_ALLOC) {
				const bool writable   = hdr->sh_flags & SHF_WRITE;
				const bool executable = hdr->sh_flags & SHF_EXECINSTR;

				const auto*  src = binary.data() + hdr->sh_offset;
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
		printf("* Entry is at %p\n", (void*) (uintptr_t) this->start_address());
	}

}
