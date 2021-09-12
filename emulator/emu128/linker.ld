OUTPUT_FORMAT("elf64-littleriscv")
MEMORY
{
    ram : ORIGIN = 0x100000, LENGTH = 0xA000
}
SECTIONS
{
    .text : { *(.text*) } > ram
    .rodata : { *(.rodata*) } > ram
    .bss : { *(.bss*) } > ram
}
