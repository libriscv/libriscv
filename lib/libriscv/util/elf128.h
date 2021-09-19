#ifndef ELF128_H
#define ELF128_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "elf.h"

typedef uint16_t    Elf128_Half;
typedef uint32_t    Elf128_Word;
typedef uint64_t    Elf128_Xword;
typedef __uint128_t Elf128_Addr;
typedef uint64_t    Elf128_Off;

#define ELFCLASS128  3

typedef struct {
  unsigned char	e_ident[EI_NIDENT];
  Elf128_Half	e_type;
  Elf128_Half	e_machine;
  Elf128_Word	e_version;
  Elf128_Addr	e_entry;
  Elf128_Off	e_phoff;
  Elf128_Off	e_shoff;
  Elf128_Word	e_flags;
  Elf128_Half	e_ehsize;
  Elf128_Half	e_phentsize;
  Elf128_Half	e_phnum;
  Elf128_Half	e_shentsize;
  Elf128_Half	e_shnum;
  Elf128_Half	e_shstrndx;
} Elf128_Ehdr;

typedef struct {
  Elf128_Word	sh_name;
  Elf128_Word	sh_type;
  Elf128_Xword	sh_flags;
  Elf128_Addr	sh_addr;
  Elf128_Off	sh_offset;
  Elf128_Xword	sh_size;
  Elf128_Word	sh_link;
  Elf128_Word	sh_info;
  Elf128_Xword	sh_addralign;
  Elf128_Xword	sh_entsize;
} Elf128_Shdr;

typedef struct {
  Elf128_Word	p_type;
  Elf128_Word	p_flags;
  Elf128_Off	p_offset;
  Elf128_Addr	p_vaddr;
  Elf128_Addr	p_paddr;
  Elf128_Xword	p_filesz;
  Elf128_Xword	p_memsz;
  Elf128_Xword	p_align;
} Elf128_Phdr;

typedef struct {
  Elf128_Word	st_name;
  unsigned char	st_info;
  unsigned char st_other;
  Elf128_Half	st_shndx;
  Elf128_Addr	st_value;
  Elf128_Xword	st_size;
} Elf128_Sym;

#ifdef __cplusplus
}
#endif

#endif /* ELF128_H */
