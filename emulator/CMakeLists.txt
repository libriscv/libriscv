cmake_minimum_required(VERSION 3.9)
project(riscv CXX)

option(LTO         "Enable interprocedural optimizations" ON)
option(NATIVE      "Enable native instructions" ON)
option(SANITIZE    "Enable sanitizers" OFF)
option(GPROF       "Enable profiling with gprof" OFF)
option(LINKER_GC   "Enable linker section garbage collection" OFF)

set(SOURCES
	src/main.cpp
)

set(CMAKE_CXX_FLAGS "-O3")
if (RISCV_DEBUG)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O1")
endif()
if (SANITIZE)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address,undefined")
endif()
if (NATIVE)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native")
endif()
if (GPROF)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pg -g -fno-inline")
endif()

# GC-sections
if (LINKER_GC)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffunction-sections" "-fdata-sections")
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-gc-sections")
endif()

add_subdirectory(../lib lib)
add_subdirectory(syscalls)

if (LTO)
	set_target_properties(riscv PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
	if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
		set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld")
	endif()
endif()

function (add_emulator NAME)
	add_executable(${NAME} ${SOURCES})
	target_link_libraries(${NAME} riscv syscalls)
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
