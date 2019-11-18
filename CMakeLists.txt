cmake_minimum_required(VERSION 3.9)
project(riscv CXX)

option(LTO         "Enable interprocedural optimizations" ON)
option(NATIVE      "Enable native instructions" ON)
option(SANITIZE    "Enable sanitizers" OFF)
option(GPROF       "Enable profiling with gprof" OFF)

add_subdirectory(lib)

set(SOURCES
	src/main.cpp
	src/linux.cpp
	src/syscalls.cpp
	src/threads.cpp
)

add_executable(remu ${SOURCES})
target_link_libraries(remu riscv)
set_target_properties(remu PROPERTIES CXX_STANDARD 17)

if (LTO)
	set_target_properties(riscv PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
	set_property(TARGET remu PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()
if (SANITIZE)
	target_compile_options(riscv PUBLIC "-fsanitize=address,undefined")
	target_link_libraries(remu "-fsanitize=address,undefined")
endif()
if (NATIVE)
	target_compile_options(riscv PUBLIC "-march=native")
endif()
target_compile_options(riscv PUBLIC -O2)
if (GPROF)
	target_compile_options(riscv PUBLIC -pg -g)
	target_link_libraries(remu "-pg")
endif()

# GC-sections
target_compile_options(riscv PUBLIC "-ffunction-sections" "-fdata-sections")
target_link_libraries(remu "-Wl,-gc-sections")
