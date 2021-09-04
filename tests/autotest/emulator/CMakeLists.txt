cmake_minimum_required(VERSION 3.9)
project(emulator CXX)

add_subdirectory(../../lib lib)

set(SOURCES
	src/main.cpp
	src/syscalls.cpp
)

set_target_properties(riscv PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld")
endif()
target_compile_options(riscv PUBLIC "-march=native")
target_compile_options(riscv PUBLIC -O3 -Wall -Wextra)

set(NAME emulator)
add_executable(${NAME} ${SOURCES})
target_link_libraries(${NAME} riscv)
set_target_properties(${NAME} PROPERTIES CXX_STANDARD 17)
target_include_directories(${NAME} PRIVATE .)

set_property(TARGET ${NAME} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
