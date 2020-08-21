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

# GC-sections
if (LINKER_GC)
	target_compile_options(riscv PUBLIC "-ffunction-sections" "-fdata-sections")
	target_link_libraries(riscv "-Wl,-gc-sections")
endif()

function (add_emulator NAME)
	add_executable(${NAME} ${SOURCES})
	target_link_libraries(${NAME} riscv)
	target_link_libraries(${NAME} syscalls)
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

add_emulator(rvlinux  EMULATOR_MODE_LINUX=1)
add_emulator(rvnewlib EMULATOR_MODE_NEWLIB=1)
add_emulator(rvmicro  EMULATOR_MODE_MICRO=1)

add_emulator(rvlinux64  EMULATOR_MODE_LINUX=1 EMULATOR_MODE_64BIT=1)
add_emulator(rvnewlib64 EMULATOR_MODE_NEWLIB=1 EMULATOR_MODE_64BIT=1)
add_emulator(rvmicro64  EMULATOR_MODE_MICRO=1 EMULATOR_MODE_64BIT=1)
