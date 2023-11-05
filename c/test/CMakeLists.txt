cmake_minimum_required(VERSION 3.9.4)
project(riscv)

add_subdirectory(.. riscv)

add_executable(test test.c)
target_link_libraries(test riscv_capi)
