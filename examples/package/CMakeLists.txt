cmake_minimum_required(VERSION 3.5)
project(example LANGUAGES CXX)

find_package(PkgConfig)
pkg_check_modules(libriscv REQUIRED)

add_executable(example example.cpp)
target_link_libraries(example riscv)
