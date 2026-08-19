cmake_minimum_required(VERSION 3.9)
project(emulator CXX)

option(RISCV_EXT_C "" OFF)
option(RISCV_EXT_V "" OFF)
add_subdirectory(../../lib libriscv)

set(SOURCES
	ibench.cpp
)

add_executable(ibench ${SOURCES})
target_link_libraries(ibench riscv)
target_include_directories(ibench PRIVATE .)
