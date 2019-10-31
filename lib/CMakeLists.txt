#
# C++17 RISC-V emulator library
#

option(RISCV_DEBUG "Enable debugging features in the RISC-V machine" OFF)

set (SOURCES
		libriscv/cpu.cpp
		libriscv/memory.cpp
		libriscv/rv32i.cpp
	)
if (RISCV_DEBUG)
	list(APPEND SOURCES
		libriscv/debug.cpp
	)
endif()


add_library(riscv ${SOURCES})
set_target_properties(riscv PROPERTIES CXX_STANDARD 17)
target_include_directories(riscv PUBLIC .)
target_compile_options(riscv PUBLIC -g -O2)
if (RISCV_DEBUG)
	target_compile_definitions(riscv PUBLIC RISCV_DEBUG=1)
endif()
