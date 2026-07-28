cmake_minimum_required(VERSION 3.14)
project(libriscv_attribute_bench LANGUAGES CXX)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../lib libriscv)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(attrbench
	src/script.cpp
	src/flat.cpp
	src/zerocopy.cpp
	src/main.cpp
)
target_compile_options(attrbench PRIVATE -Wall -Wextra)
target_link_libraries(attrbench riscv)
