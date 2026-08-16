cmake_minimum_required(VERSION 3.10)
project(libriscv_disasm CXX)

set(CMAKE_CXX_FLAGS "-Wall -Wextra -O1 -ggdb3")

# The verifier compares libriscv's decoding against binutils across the whole
# encoding space, so every extension whose printers we want covered must be on.
#
# With V on, libriscv claims funct3 0/5/6/7 of the FP load and store opcodes for
# the vector forms, which is why the reference ISA in corpus.py carries a `v`.
option(RISCV_EXT_A "" ON)
option(RISCV_EXT_C "" ON)
option(RISCV_EXT_V "" ON)
add_subdirectory(../../lib libriscv)

add_executable(disasm disasm.cpp)
target_link_libraries(disasm riscv)
