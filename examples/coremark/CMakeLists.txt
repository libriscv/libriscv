cmake_minimum_required(VERSION 3.14)
project(coremark-cli LANGUAGES CXX)

# Enable LTO
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)

option(RISCV_64I "" OFF)
option(RISCV_EXT_C "" OFF)
option(RISCV_MEMORY_TRAPS "" OFF)
option(RISCV_BINARY_TRANSLATION "" OFF)
add_subdirectory(../../lib libriscv)

add_executable(coremark-cli main.cpp)
target_link_libraries(coremark-cli riscv)
