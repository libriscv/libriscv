cmake_minimum_required(VERSION 3.14)
project(libriscv_expert LANGUAGES CXX)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../lib libriscv)

set(CMAKE_CXX_STANDARD 20)

add_executable(host_app src/script.cpp src/functions.cpp src/main.cpp)
target_link_libraries(host_app riscv)
