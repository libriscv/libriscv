#pragma once
#include <type_traits>
#include "util/elf.h"
#include "util/elf128.h"

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
		using Rela = Elf32_Rela;
	};

	template <>
	struct Elf<8>
	{
		using Ehdr = Elf64_Ehdr;
		using Phdr = Elf64_Phdr;
		using Shdr = Elf64_Shdr;
		using Sym  = Elf64_Sym;
		using Rela = Elf64_Rela;
	};

	template <>
	struct Elf<16>
	{
		using Ehdr = Elf128_Ehdr;
		using Phdr = Elf128_Phdr;
		using Shdr = Elf128_Shdr;
		using Sym  = Elf128_Sym;
		using Rela = Elf64_Rela;
	};

	using Elf32 = Elf<4>;
	using Elf64 = Elf<8>;
	using Elf128 = Elf<16>;

	template <typename Class>
	inline bool validate_header(const Class* hdr)
	{
		if (hdr->e_ident[EI_MAG0] != 0x7F ||
			hdr->e_ident[EI_MAG1] != 'E'  ||
			hdr->e_ident[EI_MAG2] != 'L'  ||
			hdr->e_ident[EI_MAG3] != 'F')
			return false;
		if constexpr (std::is_same_v<Class, Elf32_Ehdr>)
			return hdr->e_ident[EI_CLASS] == ELFCLASS32;
		else if constexpr (std::is_same_v<Class, Elf64_Ehdr>)
			return hdr->e_ident[EI_CLASS] == ELFCLASS64;
		else if constexpr (std::is_same_v<Class, Elf128_Ehdr>)
			return hdr->e_ident[EI_CLASS] == ELFCLASS128;
		return false;
	}
}
