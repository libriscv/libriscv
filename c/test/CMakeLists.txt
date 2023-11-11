cmake_minimum_required(VERSION 3.9.4)
project(riscv)

# Enable read-write arena, and disable memory traps
option(RISCV_EXPERIMENTAL "" ON)
option(RISCV_FLAT_RW_ARENA "" ON)
add_subdirectory(.. riscv)

add_executable(test test.c)
target_link_libraries(test riscv_capi)
