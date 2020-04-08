cmake_minimum_required(VERSION 3.9)
project(riscv CXX)

option(LTO         "Enable interprocedural optimizations" OFF)
option(NATIVE      "Enable native instructions" ON)
option(SANITIZE    "Enable sanitizers" OFF)
option(GPROF       "Enable profiling with gprof" OFF)
option(LINKER_GC   "Enable linker section garbage collection" OFF)

add_subdirectory(../lib lib)
add_subdirectory(syscalls)

set(SOURCES
	src/main.cpp
	src/linux.cpp
)

add_executable(remu ${SOURCES})
target_link_libraries(remu riscv)
target_link_libraries(remu syscalls)
target_compile_options(remu PUBLIC "-std=c++17")

if (LTO)
	set_target_properties(riscv PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
	set_property(TARGET remu PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
	if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
		set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld")
	endif()
endif()
if (SANITIZE)
	target_compile_options(riscv PUBLIC "-fsanitize=address,undefined")
	target_link_libraries(remu "-fsanitize=address,undefined")
endif()
if (NATIVE)
	target_compile_options(riscv PUBLIC "-march=native")
endif()
target_compile_options(riscv PUBLIC -O3 -Wall -Wextra)
if (GPROF)
	target_compile_options(riscv PUBLIC -pg -g -fno-inline)
	target_link_libraries(remu "-pg")
endif()

# GC-sections
if (LINKER_GC)
	target_compile_options(riscv PUBLIC "-ffunction-sections" "-fdata-sections")
	target_link_libraries(remu "-Wl,-gc-sections")
endif()
