#pragma once
#include "util/elf.h"

namespace riscv
{
	template <int W>
	struct Elf;

	template <>
	struct Elf<4>
	{
		using Ehdr = Elf32_Ehdr;
		using Phdr = Elf32_Phdr;
		using Shdr = Elf32_Shdr;
		using Sym  = Elf32_Sym;
	};

	template <>
	struct Elf<8>
	{
		using Ehdr = Elf64_Ehdr;
		using Phdr = Elf64_Phdr;
		using Shdr = Elf64_Shdr;
		using Sym  = Elf64_Sym;
	};

	using Elf32 = Elf<4>;
	using Elf64 = Elf<8>;

	template <typename Class>
	inline bool validate_header(const Class* hdr)
	{
		return	hdr->e_ident[0] == 0x7F &&
				hdr->e_ident[1] == 'E'  &&
				hdr->e_ident[2] == 'L'  &&
				hdr->e_ident[3] == 'F';
	}
}
