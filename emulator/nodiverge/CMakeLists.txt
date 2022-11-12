cmake_minimum_required(VERSION 3.9.4)
project(riscv CXX)

set(SOURCES
	nodiverge.cpp
	workarounds.cpp
)

add_compile_options(-fsanitize=address,undefined)
add_link_options(-fsanitize=address,undefined)

option(RISCV_DEBUG "" ON)
add_subdirectory(../../lib lib)

add_executable(nodiverge ${SOURCES})
target_link_libraries(nodiverge riscv)
