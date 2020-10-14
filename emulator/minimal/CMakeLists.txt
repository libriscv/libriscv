cmake_minimum_required(VERSION 3.9)
project(emulator CXX)

option(LTO         "Enable interprocedural optimizations" OFF)
option(NATIVE      "Enable native instructions" ON)
option(SANITIZE    "Enable sanitizers" OFF)
option(GPROF       "Enable profiling with gprof" OFF)

add_subdirectory(../../lib lib)

set(SOURCES
	src/main.cpp
)

if (LTO)
	set_target_properties(riscv PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
	if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
		set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld")
	endif()
endif()
if (SANITIZE)
	target_compile_options(riscv PUBLIC "-fsanitize=address,undefined")
endif()
if (NATIVE)
	target_compile_options(riscv PUBLIC "-march=native")
endif()
target_compile_options(riscv PUBLIC -O3 -Wall -Wextra)
if (GPROF)
	target_compile_options(riscv PUBLIC -pg -g -fno-inline)
endif()

function (add_emulator NAME)
	add_executable(${NAME} ${SOURCES})
	target_link_libraries(${NAME} riscv)
	set_target_properties(${NAME} PROPERTIES CXX_STANDARD 17)
	message(STATUS "ARGN is ${ARGN}")
	target_compile_definitions(${NAME} PRIVATE ${ARGN})

	if (LTO)
		set_property(TARGET ${NAME} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
	endif()
	if (SANITIZE)
		target_link_libraries(${NAME} "-fsanitize=address,undefined")
	endif()
	if (GPROF)
		target_link_libraries(${NAME} "-pg")
	endif()

endfunction()

add_emulator(emulator)
