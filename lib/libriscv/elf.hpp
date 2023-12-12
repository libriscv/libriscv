#pragma once
#include <string_view>
#include <type_traits>
#include "util/elf.h"
#ifdef RISCV_128BIT_ISA
#include "util/elf128.h"
#endif

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
		using Dyn  = Elf32_Dyn;
	};

	template <>
	struct Elf<8>
	{
		using Ehdr = Elf64_Ehdr;
		using Phdr = Elf64_Phdr;
		using Shdr = Elf64_Shdr;
		using Sym  = Elf64_Sym;
		using Rela = Elf64_Rela;
		using Dyn  = Elf64_Dyn;
	};

	using Elf32 = Elf<4>;
	using Elf64 = Elf<8>;

#ifdef RISCV_128BIT_ISA
	template <>
	struct Elf<16>
	{
		using Ehdr = Elf128_Ehdr;
		using Phdr = Elf128_Phdr;
		using Shdr = Elf128_Shdr;
		using Sym  = Elf128_Sym;
		using Rela = Elf64_Rela;
		using Dyn  = Elf64_Dyn;
	};
	using Elf128 = Elf<16>;
#endif

	template <typename Class>
	inline bool validate_header(std::string_view binary)
	{
		if (binary.size() < sizeof(Class))
			return false;
		auto* hdr = (Class *)binary.data();
		if (hdr->e_ident[EI_MAG0] != 0x7F ||
			hdr->e_ident[EI_MAG1] != 'E'  ||
			hdr->e_ident[EI_MAG2] != 'L'  ||
			hdr->e_ident[EI_MAG3] != 'F')
			return false;
		if constexpr (std::is_same_v<Class, Elf32_Ehdr>)
			return hdr->e_ident[EI_CLASS] == ELFCLASS32;
		else if constexpr (std::is_same_v<Class, Elf64_Ehdr>)
			return hdr->e_ident[EI_CLASS] == ELFCLASS64;
#ifdef RISCV_128BIT_ISA
		else if constexpr (std::is_same_v<Class, Elf128_Ehdr>)
			return hdr->e_ident[EI_CLASS] == ELFCLASS128;
#endif
		return false;
	}
}
