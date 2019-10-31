cmake_minimum_required(VERSION 3.9)
project(riscv C CXX)

option(LTO         "Enable interprocedural optimizations" OFF)
option(NATIVE      "Enable native instructions" OFF)
option(SANITIZE    "Enable sanitizers" OFF)

add_subdirectory(lib)

set(SOURCES
	src/main.cpp
	src/linux.cpp
	src/syscalls.cpp
)

add_executable(remu ${SOURCES})
target_link_libraries(remu riscv)
set_target_properties(remu PROPERTIES CXX_STANDARD 17)

if (LTO)
	set_property(TARGET remu  PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()
if (SANITIZE)
	target_compile_options(riscv PUBLIC "-fsanitize=address,undefined")
	target_link_libraries(remu "-fsanitize=address,undefined")
endif()
if (NATIVE)
	target_compile_options(riscv PUBLIC "-march=native")
endif()
