cmake_minimum_required(VERSION 3.14)
project(libriscv_rust_attribute_bench LANGUAGES CXX)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../lib libriscv)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(rust_attrbench
	src/script.cpp
	src/flat.cpp
	src/rustattrs.cpp
	src/probe.cpp
	src/main.cpp
)
target_compile_options(rust_attrbench PRIVATE -Wall -Wextra)
target_link_libraries(rust_attrbench riscv)
