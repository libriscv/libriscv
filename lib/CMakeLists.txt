#
# C++17 RISC-V emulator library
#

set (SOURCES
		libriscv/cpu.cpp
		libriscv/memory.cpp
		libriscv/rv32i.cpp
	)


add_library(riscv ${SOURCES})
set_target_properties(riscv PROPERTIES CXX_STANDARD 17)
target_include_directories(riscv PUBLIC .)
