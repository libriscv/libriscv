#pragma once
#include <string_view>
#include <type_traits>
#include "types.hpp"

namespace riscv
{
	static constexpr unsigned ELFCLASS32  = 1;
	static constexpr unsigned ELFCLASS64  = 2;
	static constexpr unsigned ELFCLASS128 = 3;

	template <int W>
	struct Elf
	{
		using addr_t  = address_type<W>;
		using saddr_t = signed_address_type<W>;

		static constexpr uint32_t PT_LOAD    = 1;
		static constexpr uint32_t PT_DYNAMIC = 2;
		static constexpr uint32_t PT_PHDR	 = 6;
		static constexpr uint32_t PT_GNU_EH_FRAME = 0x6474e550;
		static constexpr uint32_t PT_GNU_STACK = 0x6474e551;
		static constexpr uint32_t PT_GNU_RELRO = 0x6474e552;

		static constexpr unsigned PF_X = 1 << 0;
		static constexpr unsigned PF_W = 1 << 1;
		static constexpr unsigned PF_R = 1 << 2;

		static constexpr unsigned STT_NOTYPE = 0;
		static constexpr unsigned STT_OBJECT = 1;
		static constexpr unsigned STT_FUNC   = 2;

		static constexpr unsigned STB_LOCAL  = 0;
		static constexpr unsigned STB_GLOBAL = 1;
		static constexpr unsigned STB_WEAK   = 2;

		struct Header {
			static constexpr unsigned EI_NIDENT = 16;
			static constexpr unsigned EI_CLASS  = 4;
			static constexpr unsigned ET_EXEC   = 2;
			static constexpr unsigned ET_DYN    = 3;
			static constexpr unsigned EM_RISCV  = 243;

			unsigned char	e_ident[EI_NIDENT];
			uint16_t	e_type;
			uint16_t	e_machine;
			uint32_t	e_version;
			addr_t		e_entry;
			addr_t		e_phoff;
			addr_t		e_shoff;
			uint32_t	e_flags;
			uint16_t	e_ehsize;
			uint16_t	e_phentsize;
			uint16_t	e_phnum;
			uint16_t	e_shentsize;
			uint16_t	e_shnum;
			uint16_t	e_shstrndx;
		};

		struct SectionHeader {
			uint32_t	sh_name;
			uint32_t	sh_type;
			addr_t		sh_flags;
			addr_t		sh_addr;
			addr_t		sh_offset;
			addr_t		sh_size;
			uint32_t	sh_link;
			uint32_t	sh_info;
			addr_t		sh_addralign;
			addr_t		sh_entsize;
		};

		struct Phdr32 {
			uint32_t	p_type;
			uint32_t	p_offset;
			uint32_t	p_vaddr;
			uint32_t	p_paddr;
			uint32_t	p_filesz;
			uint32_t	p_memsz;
			uint32_t	p_flags;
			uint32_t	p_align;
		};
		struct Phdr64 {
			uint32_t	p_type;
			uint32_t	p_flags;
			uint64_t	p_offset;
			uint64_t	p_vaddr;
			uint64_t	p_paddr;
			uint64_t	p_filesz;
			uint64_t	p_memsz;
			uint64_t	p_align;
		};
#ifdef RISCV_128I
		struct Phdr128 {
			uint32_t	p_type;
			uint32_t	p_flags;
			addr_t		p_offset;
			addr_t		p_vaddr;
			addr_t		p_paddr;
			addr_t		p_filesz;
			addr_t		p_memsz;
			addr_t		p_align;
		};
		using ProgramHeader = typename std::conditional<W == 4, Phdr32, typename std::conditional<W == 8, Phdr64, Phdr128>::type>::type;
#else
		using ProgramHeader = typename std::conditional<W == 4, Phdr32, Phdr64>::type;
#endif

		struct Sym32 {
			uint32_t	st_name;
			uint32_t	st_value;
			uint32_t	st_size;
			unsigned char	st_info;
			unsigned char	st_other;
			uint16_t	st_shndx;
		};
		struct Sym64 {
			uint32_t	st_name;
			unsigned char	st_info;
			unsigned char	st_other;
			uint16_t	st_shndx;
			uint64_t	st_value;
			uint64_t	st_size;
		};
		using Sym = typename std::conditional<W == 4, Sym32, Sym64>::type;

		struct Rela {
			addr_t	r_offset;
			addr_t	r_info;
			saddr_t r_addend;
		};

		static bool validate(std::string_view binary);

		/// @brief Check if the binary is a dynamic executable
		/// @param binary ELF binary
		/// @return true if the binary is a dynamic executable, as well as the interpreter
		static std::tuple<bool, std::string_view> is_dynamic(std::string_view binary);

		static unsigned SymbolType(uint8_t st_info) {
			return st_info & 0xF;
		}
		static unsigned SymbolBind(uint8_t st_info) {
			return st_info >> 4;
		}

		static unsigned RelaSym(addr_t r_info) {
			if constexpr (W == 4)
				return r_info >> 8;
			else
				return r_info >> 32;
		}

	};

	template <int W>
	inline bool Elf<W>::validate(std::string_view binary)
	{
		if (binary.size() < sizeof(Header))
			return false;
		auto& hdr = *(Header *)binary.data();
		if (hdr.e_ident[0] != 0x7F ||
			hdr.e_ident[1] != 'E'  ||
			hdr.e_ident[2] != 'L'  ||
			hdr.e_ident[3] != 'F')
			return false;
		if constexpr (W == 4)
			return hdr.e_ident[Header::EI_CLASS] == ELFCLASS32;
		else if constexpr (W == 8)
			return hdr.e_ident[Header::EI_CLASS] == ELFCLASS64;
		else if constexpr (W == 16)
			return hdr.e_ident[Header::EI_CLASS] == ELFCLASS128;
		return false;
	}

	template <int W>
	inline std::tuple<bool, std::string_view> Elf<W>::is_dynamic(std::string_view binary)
	{
		auto* hdr = (Header *)binary.data();
		if (binary.size() < sizeof(Header))
			return {false, ""};

		// Check if it is a 64-bit executable with an .interp section
		if ((W == 4 && binary[4] == riscv::ELFCLASS32) || (W == 8 && binary[4] == riscv::ELFCLASS64))
		{
			if (hdr->e_shoff + hdr->e_shnum * sizeof(SectionHeader) > binary.size())
#if __cpp_exceptions
				throw riscv::MachineException(riscv::INVALID_PROGRAM, "Invalid ELF section headers");
#else
				return {false, ""};
#endif
			if (hdr->e_shstrndx == 0 || hdr->e_shstrndx >= hdr->e_shnum)
#if __cpp_exceptions
				throw riscv::MachineException(riscv::INVALID_PROGRAM, "Invalid ELF section header string table index");
#else
				return {false, ""};
#endif
			// If the .interp section is present, it is a dynamic executable
			// This is not part of the sandbox, so just go for it
			auto* sections = (SectionHeader *)(binary.data() + hdr->e_shoff);
			auto* sections_end = sections + hdr->e_shnum;

			if (sections[hdr->e_shstrndx].sh_offset + sections[hdr->e_shstrndx].sh_size > binary.size())
#if __cpp_exceptions
				throw riscv::MachineException(riscv::INVALID_PROGRAM, "Invalid ELF section header string table");
#else
				return {false, ""};
#endif
			auto* shstrtab = (char *)(binary.data() + sections[hdr->e_shstrndx].sh_offset);
			for (auto* s = sections; s < sections_end; s++)
			{
				std::string_view name = &shstrtab[s->sh_name];
				if (name == ".interp")
				{
					if (s->sh_offset + s->sh_size > binary.size())
#if __cpp_exceptions
						throw riscv::MachineException(riscv::INVALID_PROGRAM, "Invalid ELF .interp section");
#else
						return {false, ""};
#endif
					return {true, std::string_view(binary.data() + s->sh_offset, s->sh_size)};
				}
			}
		}

		return {false, ""};
	}
}
